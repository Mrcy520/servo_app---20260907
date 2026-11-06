/**
 ******************************************************************************
 * @file    dbg_report.c
 * @brief   调试上报层实现 · 按来源分流到不同 RTT 上行通道
 *
 * 通道分配（调试器里各占一个终端窗口）：
 *   LOG_CH_MCU   [BOOT] [PR] [EV] [ST]   MCU 自身：启动、参数、事件、统计
 *   LOG_CH_FPGA  [FP] [ST]               FPGA 内部接口：实时量、SPI3 链路统计
 *   LOG_CH_COMM  [DN] [UP]               外部协议：上下行报文（含线上 hex）
 *   LOG_CH_ANGLE 15bit:279.061,18bit:317.743  角度单值流：同 raw 双口径，专供比对
 *
 * 输出样例（默认 200 ms 摘要；状态/控制字/参数变化时即时输出；1 Hz 统计）：
 *   [DN] t= 1234ms ctrl=0x01(EN) mode=VOLT q=+10.00% d=+0.00% | AA 55 01 01 06 66 00 00 00 6D
 *   [UP] t= 1234ms st=0x20(EN) ang=359.997 iq=+0.00A vbus=28.0V | AA 55 20 FF FF 80 FF FF 38 xx
 *   [FP] t= 1234ms st=0x01 vbus=28.0V ang=359.997 iq=+0.000A enc=0x05
 *   [PR] t= 1234ms READ idx=0x09(VGAIN) st=0x00(OK) val=193
 *   [ST] dn=1234 pr=3 ctrl_err=0 param_err=0 t/o=0 tx_drop=0 | rtt_drop 0/0/0/0
 *   [ANG] raw=0x0A1234 min=0x000000 max=0x0B3FFF span=734207 | 1/2^15=323.251 ...
 *   [RAW] AA 55 01 09 F1 0A 12 34 FF ...   32 字节原始接收帧
 *   15bit:279.061,18bit:317.743        <- ch3，500 Hz，每行一个样本（双口径）
 *
 * 设计约束：
 *   1. 所有输出只在 Dbg_Poll()（主循环）里发生，钩子仅做结构体快照并置事件标志，
 *      避免在 1 kHz 路径与中断上下文里做格式化。
 *   2. 上行数据由本层每轮主动向 CommApp_BuildUpFrame 取，不依赖协议发送路径，
 *      因此 COMM_DEBUG_SILENT=1（协议静默）时依然能看到 [UP]。
 *   3. 每行在本地拼装后一次性提交（见 rtt_log.h 的行原子性说明），
 *      缓冲区不足时整行丢弃，不会出现半行。
 *   4. 只做展示，不改变任何控制行为。
 ******************************************************************************
 */

#include "dbg_report.h"
#include "rtt_log.h" /* 硬故障上报也用它，故放在调试开关之外 */

#if (COMM_DEBUG_REPORT != 0)

#include <string.h>

#include "comm_app.h"
#include "comm_proto.h"
#include "fpga_link.h"
#include "fpga_app.h"
#include "nvm.h" /* g_stat_nvm_err，仅用于 [ST] 统计行 */

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */
static ctrl_up_t   s_up; /* 每轮从 CommApp_BuildUpFrame 取最新值 */
static ctrl_down_t s_dn;
static fpga_rx_t   s_fp;
static param_frame_t s_req;
static param_frame_t s_rsp;

static uint8_t s_dn_valid;
static uint8_t s_fp_valid;
static uint8_t s_pr_valid;

static uint32_t s_dn_cnt;
static uint32_t s_pr_cnt;

static uint32_t s_evt_tick;  /* 上次事件打印时刻 */
static uint32_t s_prd_tick;  /* 上次周期摘要时刻 */
static uint32_t s_stat_tick; /* 上次统计行时刻 */
static uint32_t s_ang_tick;  /* 上次角度采样时刻（独立节拍，不受事件 return 影响） */

/* 角度 raw 的上电以来极值：用于判断"是换算错还是行程就这样" */
static uint32_t s_ang_min;
static uint32_t s_ang_max;
static uint8_t  s_ang_seen;
static uint8_t  s_evt;       /* 有事件待打印 */
static uint8_t  s_dn_evt;    /* 本次事件由"收到新下行帧"引起，才打印 [DN] */
static uint8_t  s_pr_evt;    /* 有参数读写事件待打印 */

static uint8_t s_prev_st;   /* 上行状态位变化检测 */
static uint8_t s_prev_ctrl; /* 下行控制字变化检测 */
static uint8_t s_prev_mode;

static const char* const k_st_name[8] = {"ANG", "OC", "OV", "UV", "OT", "EN", "LOST", "LATCH"};

/* -------------------------------------------------------------------------- */
/* 字段拼装（全部作用于调用方的行缓冲，不直接输出）                             */
/* -------------------------------------------------------------------------- */

static void Dbg_AddHex(log_line_t* ln, const uint8_t* p, uint8_t n)
{
    Log_LineAddHex(ln, p, (unsigned)n);
}

static void Dbg_AddStatus(log_line_t* ln, uint8_t st)
{
    uint8_t i;
    uint8_t first = 1u;

    if (st == 0u) {
        Log_LineAdd(ln, "ok");
        return;
    }
    for (i = 0u; i < 8u; i++) {
        if ((st & (uint8_t)(1u << i)) != 0u) {
            Log_LineAdd(ln, "%s%s", (first != 0u) ? "" : "|", k_st_name[i]);
            first = 0u;
        }
    }
}

static void Dbg_AddCtrl(log_line_t* ln, uint8_t ctrl)
{
    static const char* const k_cw_name[4] = {"EN", "CLR", "ZERO", "LATCH"};
    uint8_t i;
    uint8_t first = 1u;

    if (ctrl == 0u) {
        Log_LineAdd(ln, "idle");
        return;
    }
    for (i = 0u; i < 4u; i++) {
        if ((ctrl & (uint8_t)(1u << i)) != 0u) {
            Log_LineAdd(ln, "%s%s", (first != 0u) ? "" : "|", k_cw_name[i]);
            first = 0u;
        }
    }
}

static const char* Dbg_ModeName(uint8_t mode)
{
    if (mode == MODE_VOLTAGE) {
        return "VOLT";
    }
    if (mode == MODE_CURRENT) {
        return "ILOOP";
    }
    return "?";
}

static const char* Dbg_ParamName(uint8_t idx)
{
    switch (idx) {
        case PI_POLE_PAIRS: return "POLE";
        case PI_MOTOR_DIR: return "MDIR";
        case PI_ENC_DIR: return "EDIR";
        case PI_IQ_KP: return "KP_Q";
        case PI_IQ_KI: return "KI_Q";
        case PI_ID_KP: return "KP_D";
        case PI_ID_KI: return "KI_D";
        case PI_ZERO_POS: return "ZERO";
        case PI_V_GAIN: return "VGAIN";
        case PI_V_LIMIT: return "VLIM";
        case PI_COMM_TIMEOUT: return "TMOUT";
        case PI_FW_VERSION: return "FWVER";
        case PI_PROTO_VER: return "PROTO";
        default: return "?";
    }
}

static const char* Dbg_ParamStatusName(uint8_t st)
{
    switch (st) {
        case PARAM_ST_OK: return "OK";
        case PARAM_ST_UNKNOWN: return "BAD_CMD";
        case PARAM_ST_RANGE: return "RANGE";
        case PARAM_ST_INDEX: return "BAD_IDX";
        case PARAM_ST_CRC: return "CRC";
        case PARAM_ST_READONLY: return "READONLY";
        default: return "?";
    }
}

/* 百分比设定值：±32767 对应 ±100.00% */
static void Dbg_AddPct(log_line_t* ln, int16_t ref)
{
    int32_t c    = ((int32_t)ref * 10000) / 32767; /* 0.01 % */
    char    sign = (c < 0) ? '-' : '+';

    if (c < 0) {
        c = -c;
    }
    Log_LineAdd(ln, "%c%ld.%02ld%%", sign, (long)(c / 100), (long)(c % 100));
}

/* 上行角度：Uint24 对应 360/2^24 -> 0.001 度 */
static void Dbg_AddAngle24(log_line_t* ln, uint32_t ang24)
{
    uint32_t mdeg = (uint32_t)(((uint64_t)ang24 * 360000u) / 16777216u);

    Log_LineAdd(ln, "%lu.%03lu", (unsigned long)(mdeg / 1000u), (unsigned long)(mdeg % 1000u));
}

/* 上行电流：Int16 对应 0.01 A */
static void Dbg_AddCurrent16(log_line_t* ln, int16_t iq)
{
    int32_t v    = (int32_t)iq;
    char    sign = (v < 0) ? '-' : '+';

    if (v < 0) {
        v = -v;
    }
    Log_LineAdd(ln, "%c%ld.%02ldA", sign, (long)(v / 100), (long)(v % 100));
}

/* 上行母线：Uint8 对应 0.5 V */
static void Dbg_AddVbus8(log_line_t* ln, uint8_t vbus)
{
    uint32_t mv = (uint32_t)vbus * 500u;

    Log_LineAdd(ln, "%lu.%luV", (unsigned long)(mv / 1000u),
                (unsigned long)((mv % 1000u) / 100u));
}

/* -------------------------------------------------------------------------- */
/* 报文打印                                                                    */
/* -------------------------------------------------------------------------- */

static void Dbg_PrintDown(uint32_t now)
{
    log_line_t  ln;
    uint8_t     hex[CTRL_DOWN_LEN];
    ctrl_down_t dn = s_dn;

    if (s_dn_valid == 0u) {
        return;
    }
    ctrl_pack_down(hex, &dn); /* 复现线上字节，便于与抓包比对 */

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[DN] t=%5lums ctrl=0x%02X(", (unsigned long)now, dn.ctrl);
    Dbg_AddCtrl(&ln, dn.ctrl);
    Log_LineAdd(&ln, ") mode=%s q=", Dbg_ModeName(dn.mode));
    Dbg_AddPct(&ln, dn.ref1);
    Log_LineAdd(&ln, " d=");
    Dbg_AddPct(&ln, dn.ref2);
    Log_LineAdd(&ln, " | ");
    Dbg_AddHex(&ln, hex, CTRL_DOWN_LEN);
    (void)Log_LineSend(&ln, LOG_CH_COMM);
}

static void Dbg_PrintUp(uint32_t now)
{
    log_line_t ln;
    uint8_t    hex[CTRL_UP_LEN];
    ctrl_up_t  up;

    /* 上行数据源与协议发送共用同一个取数入口，保证文本与线上帧一致 */
    (void)memset(&up, 0, sizeof(up));
    CommApp_BuildUpFrame(&up);
    s_up = up;

    ctrl_pack_up(hex, &up);

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[UP] t=%5lums st=0x%02X(", (unsigned long)now, up.status);
    Dbg_AddStatus(&ln, up.status);
    Log_LineAdd(&ln, ") ang=");
    Dbg_AddAngle24(&ln, up.angle);
    Log_LineAdd(&ln, " iq=");
    Dbg_AddCurrent16(&ln, up.iq);
    Log_LineAdd(&ln, " vbus=");
    Dbg_AddVbus8(&ln, up.vbus);
    Log_LineAdd(&ln, " | ");
    Dbg_AddHex(&ln, hex, CTRL_UP_LEN);
    (void)Log_LineSend(&ln, LOG_CH_COMM);
}

static void Dbg_PrintFpga(uint32_t now)
{
    log_line_t ln;
    fpga_rx_t  rx = s_fp;
    uint32_t   mv;
    int32_t    ma;
    char       sign;

    if (s_fp_valid == 0u) {
        return;
    }

    Log_LineInit(&ln);

    if (rx.state != FPGA_ST_COMM) {
        Log_LineAdd(&ln, "[FP] t=%5lums st=0x%02X (non-realtime frame)", (unsigned long)now,
                    rx.state);
        (void)Log_LineSend(&ln, LOG_CH_FPGA);
        return;
    }

    mv = fpga_vbus_to_mv(rx.vbus_raw);
    ma = fpga_raw_to_milli(rx.iq_raw, FPGA_I_SHIFT);
    sign = (ma < 0) ? '-' : '+';
    if (ma < 0) {
        ma = -ma;
    }

    Log_LineAdd(&ln, "[FP] t=%5lums st=0x01 vbus=%lu.%luV ang=%lu.%03lu iq=%c%ld.%03ldA enc=0x%02X",
                (unsigned long)now, (unsigned long)(mv / 1000u),
                (unsigned long)((mv % 1000u) / 100u),
                (unsigned long)(fpga_angle_to_mdeg(rx.angle_raw) / 1000u),
                (unsigned long)(fpga_angle_to_mdeg(rx.angle_raw) % 1000u), sign,
                (long)(ma / 1000), (long)(ma % 1000), rx.enc_fault);
    (void)Log_LineSend(&ln, LOG_CH_FPGA);
}

/* 角度单值流（ch3）：同一 raw 按两种口径各算一次，等间隔采样，专供作图比对。
 *
 * 格式固定为   15bit:279.061,18bit:317.743
 *   15bit —— 文档定义的口径：LSB = 1/2^15 度，满圈 0xB40000（当前工程用它）
 *   18bit —— 把该字段当作 18 位编码器绝对位置：满圈 2^18 = 262144
 * 两者都只依赖同一个 rx.angle_raw，逐行可直接算出差值，便于判断哪一种与真实机械角吻合。
 * 刻意不加时间戳、不加其它字段：主机侧按逗号/冒号切分即可。
 * 只在 FPGA 返回正常实时帧（0x01）时输出，避免故障帧的残留角度混进曲线。 */
static void Dbg_PrintAngle(void)
{
    log_line_t ln;
    fpga_rx_t  rx;
    uint32_t   mdeg15;
    uint32_t   mdeg18;

    if (s_fp_valid == 0u) {
        return;
    }
    /* 链路失效时停止输出：否则最后一个有效角度会被无限重复，
     * 在曲线上表现成一条"电机停住"的水平线，比没有数据更容易误判。 */
    if (FpgaLink_Alive() == 0u) {
        return;
    }
    rx = s_fp;
    if (rx.state != FPGA_ST_COMM) {
        return;
    }

    /* 两种口径都用整数拆分输出（与 "%.3f" 逐字节一致，但不引入浮点：
     * 省掉 rtt 的浮点分支与软件 double 运算，也不在采样路径上增加几十 µs 开销）。 */
    mdeg15 = fpga_angle_to_mdeg(rx.angle_raw);
    mdeg18 = fpga_angle_to_mdeg18(rx.angle_raw);

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "15bit:%lu.%03lu,18bit:%lu.%03lu", (unsigned long)(mdeg15 / 1000u),
                (unsigned long)(mdeg15 % 1000u), (unsigned long)(mdeg18 / 1000u),
                (unsigned long)(mdeg18 % 1000u));
    (void)Log_LineSend(&ln, LOG_CH_ANGLE);
}

/* 原始报文对照（ch1）：同时打印 MOSI 上真正发出去的 32 字节与 DMA 收到的 32 字节。
 *   d=0        → RX 与 TX 逐字节相同：回环（MISO 没被 FPGA 驱动，被 MOSI 串扰/短接）
 *   d=32       → 完全不同：FPGA 在驱动 MISO，但帧格式与我们预期不符
 *   z= / f=    → 窗口里 0x00 / 0xFF 的字节数，判断"线路悬空"还是"有真实数据" */
static void Dbg_PrintRawFrame(void)
{
    log_line_t ln;
    uint8_t    win[FPGA_FRAME_LEN];
    uint8_t    tx[FPGA_FRAME_LEN];
    unsigned   n0 = 0u;
    unsigned   nf = 0u;
    unsigned   nd = 0u;
    unsigned   i;
    uint8_t    have_tx;

    if (FpgaLink_GetLastWindow(win) == 0u) {
        return;
    }
    have_tx = FpgaLink_GetLastTx(tx);

    for (i = 0u; i < FPGA_FRAME_LEN; i++) {
        if (win[i] == 0x00u) {
            n0++;
        } else if (win[i] == 0xFFu) {
            nf++;
        }
        if ((have_tx != 0u) && (win[i] != tx[i])) {
            nd++;
        }
    }

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[TX ] ");
    Log_LineAddHex(&ln, tx, FPGA_FRAME_LEN);
    (void)Log_LineSend(&ln, LOG_CH_FPGA);

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[WIN] d=%u z=%u f=%u | ", nd, n0, nf);
    Log_LineAddHex(&ln, win, FPGA_FRAME_LEN);
    (void)Log_LineSend(&ln, LOG_CH_FPGA);

    /* 若窗口以 55 AA 开头（恰好是 MCU 的发帧头），再按 16 位字节交换还原一次。
     * **只在还原后帧头/状态都合法时才打印**，避免把"不成立"的假设刷满日志。 */
    if ((win[0] == 0x55u) && (win[1] == 0xAAu)) {
        uint8_t sw[FPGA_FRAME_LEN];

        for (i = 0u; (i + 1u) < (unsigned)FPGA_FRAME_LEN; i += 2u) {
            sw[i]      = win[i + 1u];
            sw[i + 1u] = win[i];
        }

        if ((sw[0] == 0xAAu) && (sw[1] == 0x55u) && (sw[2] >= 0x01u) && (sw[2] <= 0x04u)) {
            Log_LineInit(&ln);
            Log_LineAdd(&ln, "[SWP] hdr-ok | ");
            Log_LineAddHex(&ln, sw, FPGA_FRAME_LEN);
            (void)Log_LineSend(&ln, LOG_CH_FPGA);
        }
    }
}

/* 角度量纲核对（ch1）：同一条 raw 按两种 LSB 口径各算一遍，肉眼即可判定；
 * 另给出上电以来 raw 的 min/max/span —— 若 span 明显不足满圈，
 * 说明是机械行程或编码器本身的问题，而不是换算公式的问题。 */
static void Dbg_PrintAngleDiag(void)
{
    log_line_t ln;
    fpga_rx_t  rx = s_fp;
    uint32_t   span;
    uint32_t   d15;
    uint32_t   d24;

    if (s_fp_valid == 0u) {
        return;
    }

    span = (s_ang_max >= s_ang_min) ? (s_ang_max - s_ang_min) : 0u;
    d15  = fpga_angle_to_mdeg(rx.angle_raw); /* 现行口径：1/2^15 度/LSB */
    d24  = (uint32_t)(((uint64_t)rx.angle_raw * 360000u) / 16777216u); /* 表1 总述口径 */

    Log_LineInit(&ln);
    Log_LineAdd(&ln,
                "[ANG] raw=0x%06lX min=0x%06lX max=0x%06lX span=%lu | 1/2^15=%lu.%03lu "
                "360/2^24=%lu.%03lu | fault=0x%02X over=%lu",
                (unsigned long)rx.angle_raw, (unsigned long)s_ang_min, (unsigned long)s_ang_max,
                (unsigned long)span, (unsigned long)(d15 / 1000u), (unsigned long)(d15 % 1000u),
                (unsigned long)(d24 / 1000u), (unsigned long)(d24 % 1000u), rx.enc_fault,
                (unsigned long)g_fpga_angle_over);
    (void)Log_LineSend(&ln, LOG_CH_FPGA);
}

static void Dbg_PrintParam(uint32_t now)
{
    log_line_t ln;

    if (s_pr_valid == 0u) {
        return;
    }

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[PR] t=%5lums %s idx=0x%02X(%s) st=0x%02X(%s) val=%ld", (unsigned long)now,
                (s_req.cmd == PARAM_CMD_READ) ? "READ " : "WRITE", s_req.idx,
                Dbg_ParamName(s_req.idx), s_rsp.status, Dbg_ParamStatusName(s_rsp.status),
                (long)s_rsp.value);
    (void)Log_LineSend(&ln, LOG_CH_MCU);
}
/* 上电以来极值未采样时按 0 显示，避免打印 4294967295 */
#define DBG_US(x) ((unsigned long)(((x) == 0xFFFFFFFFu) ? 0u : (x)))

/* 1 Hz 统计：MCU 侧（协议/参数/O~O）与 FPGA 侧（SPI3 链路）分通道输出 */
static void Dbg_PrintStat(void)
{
    log_line_t ln;

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[ST] dn=%lu pr=%lu ctrl_err=%lu param_err=%lu t/o=%lu tx_drop=%lu nvm_err=%lu",
                (unsigned long)s_dn_cnt, (unsigned long)s_pr_cnt, (unsigned long)g_stat_ctrl_err,
                (unsigned long)g_stat_param_err, (unsigned long)g_stat_timeout,
                (unsigned long)g_stat_tx_drop, (unsigned long)g_stat_nvm_err);
    (void)Log_LineSend(&ln, LOG_CH_MCU);

    Log_LineInit(&ln);
    Log_LineAdd(&ln,
                "[ST] cycle=%lu rx=%lu err=%lu rec=%lu e=0x%08lX slip=%lu ovr=%lu req_err=%lu nib=%lu | "
                "spi=%lu/%luus hal=%lu/%luus sck=%lukHz sync=%lu/%luus rej=%lu/%luus",
                (unsigned long)g_fpga_cycle, (unsigned long)g_fpga_rx_cnt,
                (unsigned long)g_fpga_err, (unsigned long)g_fpga_recover,
                (unsigned long)g_fpga_err_detail, (unsigned long)g_fpga_slip,
                (unsigned long)g_fpga_overrun, (unsigned long)g_fpga_req_err,
                (unsigned long)g_fpga_nib_swap,
                DBG_US(g_fpga_busy_us_min), DBG_US(g_fpga_busy_us_max),
                DBG_US(g_fpga_hal_us_min), DBG_US(g_fpga_hal_us_max),
                (unsigned long)g_fpga_sck_khz, DBG_US(g_fpga_sync_us_min),
                DBG_US(g_fpga_sync_us_max), DBG_US(g_fpga_rej_us_min),
                DBG_US(g_fpga_rej_us_max));
    (void)Log_LineSend(&ln, LOG_CH_FPGA);
}

/* 同步沿间隔分布（每 5 s 一行，只在该窗口内有样本时才打印）：
 * 稳态 16 kHz 应几乎全落在 56-72 档。若 <20 / 20-40 档占比可观，
 * 说明沿与沿之间本身就不均匀——那时压缩 SPI 窗口也不可能做到零丢帧。 */
static void Dbg_PrintSyncHist(void)
{
    log_line_t ln;
    uint16_t   h[FPGA_SYNC_HIST_N];

    if (FpgaLink_TakeSyncHist(h) == 0u) {
        return;
    }

    Log_LineInit(&ln);
    Log_LineAdd(&ln, "[SH] sync(us) <20:%u 20-40:%u 40-56:%u 56-72:%u 72-88:%u >=88:%u",
                (unsigned)h[0], (unsigned)h[1], (unsigned)h[2], (unsigned)h[3],
                (unsigned)h[4], (unsigned)h[5]);
    (void)Log_LineSend(&ln, LOG_CH_FPGA);
}

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

void Dbg_Init(void)
{
    uint32_t now = CommProto_GetTick();

    (void)memset(&s_up, 0, sizeof(s_up));
    (void)memset(&s_dn, 0, sizeof(s_dn));
    (void)memset(&s_fp, 0, sizeof(s_fp));

    s_dn_valid = 0u;
    s_fp_valid = 0u;
    s_pr_valid = 0u;
    s_dn_cnt   = 0u;
    s_pr_cnt   = 0u;

    s_evt       = 0u;
    s_evt_tick  = now;
    s_prd_tick  = now;
    s_stat_tick = now;
    s_ang_tick  = now;

    s_ang_min  = 0u;
    s_ang_max  = 0u;
    s_ang_seen = 0u;

    s_prev_st   = 0xFFu;
    s_prev_ctrl = 0xFFu;
    s_prev_mode = 0xFFu;

    Log_Printf(LOG_CH_MCU, "=== DEBUG REPORT ON (RTT, %u up ch) ===",
               (unsigned)RTT_NUM_UP_BUFFERS);
    Log_Printf(LOG_CH_MCU, "ch0/%s ch1/%s ch2/%s ch3/%s | buf %uB x%u", "MCU", "FPGA", "COMM",
               "ANGLE", (unsigned)RTT_BUFFER_SIZE_UP, (unsigned)RTT_NUM_UP_BUFFERS);
#if (COMM_DEBUG_SILENT != 0)
    Log_Printf(LOG_CH_MCU, "mode: PROTOCOL SILENT - uplink frame & param response disabled");
#else
    Log_Printf(LOG_CH_MCU, "mode: PROTOCOL ACTIVE - USART1 只跑协议，日志走 RTT 互不干扰");
#endif
    Log_Printf(LOG_CH_MCU, "tags: [DN] [UP] -> COMM | [FP] [WIN] [ANG] -> FPGA | "
                           "[PR] [EV] [ST] -> MCU");
    Log_Printf(LOG_CH_MCU, "angle stream: ch3 @ %ums -> \"15bit:279.061,18bit:317.743\"",
               DBG_ANGLE_PERIOD_MS);
}

void Dbg_OnDownFrame(const ctrl_down_t* dn)
{
    if (dn == NULL) {
        return;
    }
    s_dn       = *dn;
    s_dn_valid = 1u;
    s_dn_cnt++;

    /* 只有控制字/模式变化才算事件；给定值连续变化由周期摘要体现，避免刷屏 */
    if ((dn->ctrl != s_prev_ctrl) || (dn->mode != s_prev_mode)) {
        s_prev_ctrl = dn->ctrl;
        s_prev_mode = dn->mode;
        s_evt       = 1u;
        s_dn_evt    = 1u; /* 仅此时打印 [DN]，避免"上行状态变化"时重复打印上一帧误导 */
    }
}

void Dbg_OnFpgaFrame(const fpga_rx_t* rx)
{
    if (rx == NULL) {
        return;
    }
    s_fp       = *rx;
    s_fp_valid = 1u;

    /* 只统计实时帧：其它响应帧的字节 6-8 不是角度 */
    if (rx->state == FPGA_ST_COMM) {
        if (s_ang_seen == 0u) {
            s_ang_seen = 1u;
            s_ang_min  = rx->angle_raw;
            s_ang_max  = rx->angle_raw;
        } else {
            if (rx->angle_raw < s_ang_min) {
                s_ang_min = rx->angle_raw;
            }
            if (rx->angle_raw > s_ang_max) {
                s_ang_max = rx->angle_raw;
            }
        }
    }
}

void Dbg_OnParam(const param_frame_t* req, const param_frame_t* rsp)
{
    if ((req == NULL) || (rsp == NULL)) {
        return;
    }
    s_req = *req;
    s_rsp = *rsp;
    s_pr_cnt++;
    s_pr_valid = 1u;
    s_pr_evt   = 1u; /* 参数读写是低频事件，立即打一行 */
}

void Dbg_OnEvent(const char* tag)
{
    if (tag != NULL) {
        Log_Printf(LOG_CH_MCU, "[EV] t=%5lums %s", (unsigned long)CommProto_GetTick(), tag);
    }
}

void Dbg_Poll(void)
{
    uint32_t  now = CommProto_GetTick();
    ctrl_up_t up;

    /* 上行数据是"当前值"而非事件，每轮取一次用于状态位变化检测 */
    (void)memset(&up, 0, sizeof(up));
    CommApp_BuildUpFrame(&up);
    if (up.status != s_prev_st) {
        s_prev_st = up.status;
        s_evt     = 1u;
    }

    /* 角度单值流：独立固定节拍，必须放在事件分支的 return 之前，
     * 否则一旦有事件就直接 return，角度曲线会漏采样、时间轴变得不均匀。 */
    if ((uint32_t)(now - s_ang_tick) >= DBG_ANGLE_PERIOD_MS) {
        s_ang_tick = now;
        Dbg_PrintAngle();
    }

    /* 事件优先：变化后立即输出一组，受最小间隔限制 */
    if (((s_evt != 0u) || (s_pr_evt != 0u)) &&
        ((uint32_t)(now - s_evt_tick) >= DBG_MIN_GAP_MS)) {
        s_evt      = 0u;
        s_pr_evt   = 0u;
        s_evt_tick = now;
        s_prd_tick = now;

        Log_Printf(LOG_CH_MCU, "-- event --");
        if (s_dn_evt != 0u) {
            s_dn_evt = 0u;
            Dbg_PrintDown(now); /* 只有"收到新下行帧"才打印，避免重复显示旧帧 */
        }
        Dbg_PrintUp(now);
        Dbg_PrintFpga(now);
        Dbg_PrintParam(now);
        return;
    }

    /* 周期摘要 */
    if ((uint32_t)(now - s_prd_tick) >= DBG_PERIOD_MS) {
        s_prd_tick = now;
        Dbg_PrintDown(now);
        Dbg_PrintUp(now);
        Dbg_PrintFpga(now);
        Dbg_PrintAngleDiag(); /* 角度口径核对 */
    }

    /* 1 Hz 统计行（原始报文对照也放这里，避免 5 Hz 刷屏） */
    if ((uint32_t)(now - s_stat_tick) >= 1000u) {
        s_stat_tick = now;
        Dbg_PrintStat();
        Dbg_PrintRawFrame(); /* TX/WIN 对照 */

        /* 同步沿分布：与 [ST] 同频（1 Hz）。原来是 5 s，现场常常只抓几秒，
         * 抓不到就白等一轮——诊断数据必须每次都落在窗口里。 */
        Dbg_PrintSyncHist();
    }
}

#endif /* COMM_DEBUG_REPORT */

/* -------------------------------------------------------------------------- */
/* 硬故障上报（不受 COMM_DEBUG_REPORT 控制，永远编译）                          */
/* -------------------------------------------------------------------------- */

/*
 * 目的：现场"卡死"时至少能区分 硬故障 / 死循环，并给出故障类型与地址，
 * 而不是只剩一个 while(1) 让人猜。
 *
 * 这里直接取 ARMv7-M 架构固定的 SCB 地址，而不是 include main.h——
 * 那会让 dbg_report.c 失去"可在主机端编译"的性质。
 */
#define DBG_SCB_CFSR  (*(volatile uint32_t*)0xE000ED28u) /* 可配置故障状态   */
#define DBG_SCB_HFSR  (*(volatile uint32_t*)0xE000ED2Cu) /* 硬故障状态       */
#define DBG_SCB_MMFAR (*(volatile uint32_t*)0xE000ED34u) /* 存储管理故障地址 */
#define DBG_SCB_BFAR  (*(volatile uint32_t*)0xE000ED38u) /* 总线故障地址     */

/* 写 8 位十六进制，返回写入长度（不依赖 printf，避免把格式化器再拉进来） */
static unsigned Dbg_Hex32(char* p, uint32_t v)
{
    static const char k_hex[] = "0123456789ABCDEF";
    unsigned          i;

    for (i = 0u; i < 8u; i++) {
        p[i] = k_hex[(v >> ((7u - i) * 4u)) & 0x0Fu];
    }
    return 8u;
}

/**
 * @brief 硬故障现场上报，由 HardFault_Handler 调用
 * @note  只做寄存器读取与一次 RTT 写入，不做任何可能再次触发故障的操作；
 *        返回后由调用方的 while(1) 维持停机，便于调试器接管。
 */
void Dbg_OnHardFault(void)
{
    static const char k_tag[]  = "\r\n[FAULT] CFSR=0x";
    static const char k_name[3][10] = {" HFSR=0x", " MMFAR=0x", " BFAR=0x"};
    uint32_t          val[3];
    char              buf[96];
    unsigned          n = 0u;
    unsigned          i;
    unsigned          k;

    for (i = 0u; i < (unsigned)(sizeof(k_tag) - 1u); i++) {
        buf[n++] = k_tag[i];
    }
    n += Dbg_Hex32(&buf[n], DBG_SCB_CFSR);

    val[0] = DBG_SCB_HFSR;
    val[1] = DBG_SCB_MMFAR;
    val[2] = DBG_SCB_BFAR;

    for (k = 0u; k < 3u; k++) {
        for (i = 0u; k_name[k][i] != '\0'; i++) {
            buf[n++] = k_name[k][i];
        }
        n += Dbg_Hex32(&buf[n], val[k]);
    }

    buf[n++] = '\r';
    buf[n++] = '\n';

    (void)rtt_write_channel((unsigned)LOG_CH_MCU, buf, n);
}
