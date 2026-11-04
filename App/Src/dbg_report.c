/**
 ******************************************************************************
 * @file    dbg_report.c
 * @brief   调试上报层实现（printf -> USART1，见 Utility/Src/retarget.c）
 *
 * 输出样例（周期 200 ms 摘要；状态/控制字变化时即时打印；1 Hz 统计行）：
 *   [DN] t= 1234ms ctrl=0x01(EN) mode=VOLT q=+10.00% d=+0.00% | AA 55 01 01 06 66 00 00 00 6D
 *   [UP] t= 1234ms st=0x20(EN) ang=359.997 iq=+0.00A vbus=28.0V | AA 55 20 FF FF 80 FF FF 38 xx
 *   [FP] t= 1234ms st=0x01 vbus=28.0V ang=359.997 iq=+0.000A enc=0x05
 *   [PR] t= 1234ms READ idx=0x09(VGAIN) st=0x00(OK) val=193
 *   [ST] up=0 dn=1234 ctrl_err=0 param_err=0 t/o=0 | fpga cyc=19744 rx=19740 err=0 slip=0 ovr=0 req_err=0
 *
 * 设计约束：
 *   1. 所有 printf 只在 Dbg_Poll()（主循环）里发生，钩子仅做结构体快照并置事件标志，
 *      避免在 1 kHz 路径与中断上下文里阻塞。
 *   2. 上行数据由本层每轮主动向 CommApp_BuildUpFrame 取，不依赖协议发送路径，
 *      因此 COMM_DEBUG_SILENT=1（协议静默）时依然能看到 [UP]。
 *   3. 只做展示，不改变任何控制行为。
 ******************************************************************************
 */

#include "dbg_report.h"

#if (COMM_DEBUG_REPORT != 0)

#include <stdio.h>

#include "comm_app.h"
#include "comm_proto.h"
#include "fpga_link.h"
#include "fpga_app.h"

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
static uint8_t  s_evt;       /* 有事件待打印 */
static uint8_t  s_dn_evt;    /* 本次事件由"收到新下行帧"引起，才打印 [DN] */
static uint8_t  s_pr_evt;    /* 有参数读写事件待打印 */

static uint8_t s_prev_st;   /* 上行状态位变化检测 */
static uint8_t s_prev_ctrl; /* 下行控制字变化检测 */
static uint8_t s_prev_mode;

static const char* const k_st_name[8] = {"ANG", "OC", "OV", "UV", "OT", "EN", "LOST", "LATCH"};

/* -------------------------------------------------------------------------- */
/* 打印辅助                                                                    */
/* -------------------------------------------------------------------------- */

static void Dbg_Hex(const uint8_t* p, uint8_t n)
{
    uint8_t i;

    for (i = 0u; i < n; i++) {
        printf("%02X%s", p[i], (i + 1u < n) ? " " : "");
    }
}

static void Dbg_PrintStatus(uint8_t st)
{
    uint8_t i;
    uint8_t first = 1u;

    if (st == 0u) {
        printf("ok");
        return;
    }
    for (i = 0u; i < 8u; i++) {
        if ((st & (uint8_t)(1u << i)) != 0u) {
            printf("%s%s", (first != 0u) ? "" : "|", k_st_name[i]);
            first = 0u;
        }
    }
}

static void Dbg_PrintCtrl(uint8_t ctrl)
{
    static const char* const k_cw_name[4] = {"EN", "CLR", "ZERO", "LATCH"};
    uint8_t i;
    uint8_t first = 1u;

    if (ctrl == 0u) {
        printf("idle");
        return;
    }
    for (i = 0u; i < 4u; i++) {
        if ((ctrl & (uint8_t)(1u << i)) != 0u) {
            printf("%s%s", (first != 0u) ? "" : "|", k_cw_name[i]);
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
static void Dbg_PrintPct(int16_t ref)
{
    int32_t c    = ((int32_t)ref * 10000) / 32767; /* 0.01 % */
    char    sign = (c < 0) ? '-' : '+';

    if (c < 0) {
        c = -c;
    }
    printf("%c%ld.%02ld%%", sign, (long)(c / 100), (long)(c % 100));
}

/* 上行角度：Uint24 对应 360/2^24 -> 0.001 度 */
static void Dbg_PrintAngle24(uint32_t ang24)
{
    uint32_t mdeg = (uint32_t)(((uint64_t)ang24 * 360000u) / 16777216u);

    printf("%lu.%03lu", (unsigned long)(mdeg / 1000u), (unsigned long)(mdeg % 1000u));
}

/* 上行电流：Int16 对应 0.01 A */
static void Dbg_PrintCurrent16(int16_t iq)
{
    int32_t v    = (int32_t)iq;
    char    sign = (v < 0) ? '-' : '+';

    if (v < 0) {
        v = -v;
    }
    printf("%c%ld.%02ldA", sign, (long)(v / 100), (long)(v % 100));
}

/* 上行母线：Uint8 对应 0.5 V */
static void Dbg_PrintVbus8(uint8_t vbus)
{
    uint32_t mv = (uint32_t)vbus * 500u;

    printf("%lu.%luV", (unsigned long)(mv / 1000u), (unsigned long)((mv % 1000u) / 100u));
}

/* -------------------------------------------------------------------------- */
/* 报文打印                                                                    */
/* -------------------------------------------------------------------------- */

static void Dbg_PrintDown(uint32_t now)
{
    uint8_t     hex[CTRL_DOWN_LEN];
    ctrl_down_t dn = s_dn;

    if (s_dn_valid == 0u) {
        return;
    }
    ctrl_pack_down(hex, &dn); /* 复现线上字节，便于与抓包比对 */

    printf("[DN] t=%5lums ctrl=0x%02X(", (unsigned long)now, dn.ctrl);
    Dbg_PrintCtrl(dn.ctrl);
    printf(") mode=%s q=", Dbg_ModeName(dn.mode));
    Dbg_PrintPct(dn.ref1);
    printf(" d=");
    Dbg_PrintPct(dn.ref2);
    printf(" | ");
    Dbg_Hex(hex, CTRL_DOWN_LEN);
    printf("\r\n");
}

static void Dbg_PrintUp(uint32_t now)
{
    uint8_t    hex[CTRL_UP_LEN];
    ctrl_up_t  up;

    /* 上行数据源与协议发送共用同一个取数入口，保证文本与线上帧一致 */
    (void)memset(&up, 0, sizeof(up));
    CommApp_BuildUpFrame(&up);
    s_up = up;

    ctrl_pack_up(hex, &up);

    printf("[UP] t=%5lums st=0x%02X(", (unsigned long)now, up.status);
    Dbg_PrintStatus(up.status);
    printf(") ang=");
    Dbg_PrintAngle24(up.angle);
    printf(" iq=");
    Dbg_PrintCurrent16(up.iq);
    printf(" vbus=");
    Dbg_PrintVbus8(up.vbus);
    printf(" | ");
    Dbg_Hex(hex, CTRL_UP_LEN);
    printf("\r\n");
}

static void Dbg_PrintFpga(uint32_t now)
{
    fpga_rx_t rx = s_fp;
    uint32_t  mv;
    int32_t   ma;
    char      sign;

    if (s_fp_valid == 0u) {
        return;
    }
    if (rx.state != FPGA_ST_COMM) {
        printf("[FP] t=%5lums st=0x%02X (non-realtime frame)\r\n", (unsigned long)now, rx.state);
        return;
    }

    mv = fpga_vbus_to_mv(rx.vbus_raw);
    ma = fpga_raw_to_milli(rx.iq_raw, FPGA_I_SHIFT);
    sign = (ma < 0) ? '-' : '+';
    if (ma < 0) {
        ma = -ma;
    }

    printf("[FP] t=%5lums st=0x01 vbus=%lu.%luV ang=%lu.%03lu iq=%c%ld.%03ldA enc=0x%02X\r\n",
           (unsigned long)now, (unsigned long)(mv / 1000u), (unsigned long)((mv % 1000u) / 100u),
           (unsigned long)(fpga_angle_to_mdeg(rx.angle_raw) / 1000u),
           (unsigned long)(fpga_angle_to_mdeg(rx.angle_raw) % 1000u), sign, (long)(ma / 1000),
           (long)(ma % 1000), rx.enc_fault);
}

static void Dbg_PrintParam(uint32_t now)
{
    if (s_pr_valid == 0u) {
        return;
    }

    printf("[PR] t=%5lums %s idx=0x%02X(%s) st=0x%02X(%s) val=%ld\r\n", (unsigned long)now,
           (s_req.cmd == PARAM_CMD_READ) ? "READ " : "WRITE", s_req.idx,
           Dbg_ParamName(s_req.idx), s_rsp.status, Dbg_ParamStatusName(s_rsp.status),
           (long)s_rsp.value);
}

static void Dbg_PrintStat(void)
{
    printf("[ST] dn=%lu pr=%lu ctrl_err=%lu param_err=%lu t/o=%lu tx_drop=%lu | fpga cyc=%lu "
           "rx=%lu err=%lu slip=%lu ovr=%lu req_err=%lu\r\n",
           (unsigned long)s_dn_cnt, (unsigned long)s_pr_cnt, (unsigned long)g_stat_ctrl_err,
           (unsigned long)g_stat_param_err, (unsigned long)g_stat_timeout,
           (unsigned long)g_stat_tx_drop, (unsigned long)g_fpga_cycle,
           (unsigned long)g_fpga_rx_cnt, (unsigned long)g_fpga_err, (unsigned long)g_fpga_slip,
           (unsigned long)g_fpga_overrun, (unsigned long)g_fpga_req_err);
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

    s_prev_st   = 0xFFu;
    s_prev_ctrl = 0xFFu;
    s_prev_mode = 0xFFu;

    printf("\r\n=== DEBUG REPORT ON (USART1 921600) ===\r\n");
#if (COMM_DEBUG_SILENT != 0)
    printf("mode: PROTOCOL SILENT - uplink frame & param response disabled, text only\r\n");
#else
    printf("mode: PROTOCOL ACTIVE - text and binary share this port (mixed)\r\n");
#endif
    printf("tags: [DN] down [UP] up [FP] fpga [PR] param [ST] stat(1Hz) [EV] event\r\n\r\n");
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
        printf("[EV] t=%5lums %s\r\n", (unsigned long)CommProto_GetTick(), tag);
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

    /* 事件优先：变化后立即输出一组，受最小间隔限制 */
    if (((s_evt != 0u) || (s_pr_evt != 0u)) &&
        ((uint32_t)(now - s_evt_tick) >= DBG_MIN_GAP_MS)) {
        s_evt      = 0u;
        s_pr_evt   = 0u;
        s_evt_tick = now;
        s_prd_tick = now;

        printf("-- event --\r\n");
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
    }

    /* 1 Hz 统计行 */
    if ((uint32_t)(now - s_stat_tick) >= 1000u) {
        s_stat_tick = now;
        Dbg_PrintStat();
    }
}

#endif /* COMM_DEBUG_REPORT */
