/**
 ******************************************************************************
 * @file    fpga_app.c
 * @brief   应用层 · FPGA 执行器映射实现
 *
 * 三个方向：
 *   1. g_param   -> 0x02 参数帧：量纲与参数协议 1:1（×16、1/2^15 度），直接搬
 *   2. g_control -> 0x01 运行帧：使能/模式/找零 -> FPGA 控制模式；百分比给定
 *                              -> FPGA 的 dq 码值（LSB 1/2^16，满量程 ±30V/±10A）
 *   3. 0x01 响应 -> g_status：母线电压 0.011V -> 0.5V/LSB、角度 1/2^15 -> 360/2^24、
 *                              Q 轴电流 1/2^16 A -> 0.01A/LSB
 *
 * 请求调度（16 kHz 链路被 FPGA 独占，只能在 0x01 与 0x02/0x03/0x04 之间切换）：
 *   - 参数锁存后（g_param_gen 变化）下装一次 0x02，收到同状态应答即回到 0x01；
 *   - 每 1 s 穿插一帧 0x03 / 0x04 回读，供诊断与核对；
 *   - 单次请求 5 ms 无应答即放弃并计数，避免长时间中断实时量。
 ******************************************************************************
 */

#include "fpga_app.h"

#include "fpga_link.h"
#include "fpga_proto.h"
#include "comm_app.h"
#include "dbg_report.h" /* 调试上报（编译期可关） */

#include <string.h>

/* -------------------------------------------------------------------------- */
/* 调度参数                                                                    */
/* -------------------------------------------------------------------------- */
#define FPGA_APP_TICK_MS        1u    /* 应用层节拍，与外部 1 kHz 下行同频 */
#define FPGA_APP_REQ_TIMEOUT_MS 5u    /* 非实时请求等待应答的超时（≈80 个 16kHz 周期） */
#define FPGA_APP_POLL_MS        1000u /* 0x03/0x04 回读周期 */
#define FPGA_APP_CMD_REFRESH_MS 1u    /* 命令帧重建周期 */

typedef enum {
    FPGA_REQ_NONE = 0,   /* 正常通讯（0x01），携带运行指令 */
    FPGA_REQ_LOAD_PARAM, /* 0x02 下装参数 */
    FPGA_REQ_READ_PARAM, /* 0x03 回读参数 */
    FPGA_REQ_READ_DATA   /* 0x04 回读内部数据 */
} fpga_req_t;

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */
volatile uint32_t g_fpga_req_err; /* 请求无应答次数 */
volatile uint32_t g_fpga_angle_over;

static fpga_req_t s_req;
static uint32_t s_req_tick;
static uint32_t s_req_ack_mark; /* 请求发出时的应答计数基准 */
static uint8_t s_cmd_dirty;

static uint32_t s_tick_ms;  /* 1 ms 节拍 */
static uint32_t s_cmd_tick; /* 命令帧重建计时 */
static uint32_t s_poll_tick;
static uint8_t s_poll_alt;

static uint32_t s_seen_gen;      /* 已下装的参数代次 */
static uint8_t s_enc_fault_prev; /* 编码器故障上一帧状态，用于上升沿判定 */

static fpga_cmd_t s_cmd;

/* 私有函数声明 ------------------------------------------------------------ */
static void FpgaApp_ConsumeRx(void);
static void FpgaApp_ScheduleRequest(uint32_t now);
static void FpgaApp_BuildCommand(void);
static uint8_t FpgaApp_MapMode(void);
static uint8_t FpgaApp_ReqState(fpga_req_t req);
static void FpgaApp_FillParams(fpga_cmd_t* c);

/* -------------------------------------------------------------------------- */
/* 初始化与周期任务                                                            */
/* -------------------------------------------------------------------------- */

void FpgaApp_Init(void)
{
    (void)memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.state = FPGA_ST_COMM;
    s_cmd.mode  = FPGA_MODE_WAIT;

    s_req          = FPGA_REQ_NONE;
    s_req_tick     = 0u;
    s_req_ack_mark = 0u;
    s_cmd_dirty    = 1u;

    s_tick_ms   = 0u;
    s_cmd_tick  = 0u;
    s_poll_tick = CommProto_GetTick();
    s_poll_alt  = 0u;

    /* 上电不因代次差异触发下装：默认以 FPGA 侧已有参数为准 */
    s_seen_gen       = g_param_gen;
    s_enc_fault_prev = 0u;
    g_fpga_req_err   = 0u;

    FpgaLink_Init();

    /* 上电是否下装一次参数：
     *   ① 显式打开 FPGA_APP_LOAD_ON_START；或
     *   ② 本次从内部 Flash 成功恢复了参数——此时 MCU 侧才是权威值，
     *      FPGA 上电后未必与之一致，必须下装一次，否则参数持久化等于没生效。 */
    if ((FPGA_APP_LOAD_ON_START != 0) || (CommApp_ParamsFromNvm() != 0u)) {
        s_req          = FPGA_REQ_LOAD_PARAM;
        s_req_tick     = CommProto_GetTick();
        s_req_ack_mark = g_fpga_ack_cnt;
        s_cmd_dirty    = 1u;
    }
}

void FpgaApp_Poll(void)
{
    uint32_t now = CommProto_GetTick();

    /* 16 kHz 链路本身由中断驱动，应用层按 1 ms 结算即可（外部上行帧同为 1 kHz） */
    if ((uint32_t)(now - s_tick_ms) < FPGA_APP_TICK_MS) {
        return;
    }
    s_tick_ms = now;

    FpgaApp_ConsumeRx();
    FpgaApp_ScheduleRequest(now);

    if ((s_cmd_dirty != 0u) || ((uint32_t)(now - s_cmd_tick) >= FPGA_APP_CMD_REFRESH_MS)) {
        FpgaApp_BuildCommand();
        s_cmd_tick  = now;
        s_cmd_dirty = 0u;
    }
}

uint8_t FpgaApp_LinkAlive(void)
{
    return FpgaLink_Alive();
}

/* -------------------------------------------------------------------------- */
/* 响应 -> 上行数据源                                                          */
/* -------------------------------------------------------------------------- */

static void FpgaApp_ConsumeRx(void)
{
    fpga_rx_t rx;
    uint8_t raw[FPGA_FRAME_LEN];

    /* 解码值与原始字节一次取回（同一临界区），保证调试层拿到的两者属于同一帧 */
    if (FpgaLink_GetRxEx(&rx, raw) == 0u) {
        return; /* 尚未收到任何有效帧 */
    }

    Dbg_OnFpgaFrame(&rx, raw); /* 调试打印（编译期可关） */

    if (rx.state != FPGA_ST_COMM) {
        return; /* 只有 0x01 帧携带实时量 */
    }

    /* 量纲换算到对外控制协议（见 fpga_proto.h 的换算函数） */
    g_status.vbus = fpga_vbus_to_ctrl8(rx.vbus_raw);

    /* 角度先做量纲范围校验再换算：超量程时换算函数饱和到 359.99998°，
     * 这里累计次数，供现场判定 FPGA 实际口径是否为 1/2^15。
     * 不并入 ST_ANGLE_ERR——那一位由 FPGA 的 enc_fault 独占，混入会让故障源不可辨。 */
    if (fpga_angle_in_range(rx.angle_raw) == 0u) {
        g_fpga_angle_over++;
    }
    /* 角度换算：对外统一按控制协议标准满量程（360/2^24°）上报。
     * 当前编码器硬件仅输出后 1/4 圈（对应 270°~360°），上位机将其映射为 0°~360° 用于调试 */
    g_status.angle = fpga_angle_to_mdeg_cal(rx.angle_raw);

    g_status.iq = fpga_i_raw_to_ctrl16(rx.iq_raw);

    /* 编码器故障字：
     *   ① 源位跟随电平——bit0 必须反映"当前是否报警"，否则主控清一次锁存后
     *      会在编码器仍故障时收到 st=0x00，属于状态位撒谎；
     *   ② bit7 锁存与强制关输出在 0→非零 上升沿触发，若底层故障未消解则持续保持锁存。 */
    CommApp_SetFaultLevel(ST_ANGLE_ERR, (rx.enc_fault != 0u) ? 1u : 0u);
    if ((rx.enc_fault != 0u) &&
        ((s_enc_fault_prev == 0u) || ((g_status.status & ST_FAULT_LATCH) == 0u))) {
        CommApp_SetFault(ST_ANGLE_ERR);
    }
    s_enc_fault_prev = rx.enc_fault;
}

/* -------------------------------------------------------------------------- */
/* 请求调度                                                                    */
/* -------------------------------------------------------------------------- */

static void FpgaApp_ScheduleRequest(uint32_t now)
{
    if (s_req != FPGA_REQ_NONE) {
        /* 收到与请求同状态的响应即视为完成 */
        if ((g_fpga_ack_cnt != s_req_ack_mark) && (g_fpga_ack_state == FpgaApp_ReqState(s_req))) {
            s_req       = FPGA_REQ_NONE;
            s_cmd_dirty = 1u;
            return;
        }
        if ((uint32_t)(now - s_req_tick) >= FPGA_APP_REQ_TIMEOUT_MS) {
            g_fpga_req_err++;
            s_req       = FPGA_REQ_NONE;
            s_cmd_dirty = 1u;
        }
        return;
    }

    if (g_param_gen != s_seen_gen) {
        /* 参数刚被控制字 bit3 锁存：先下装到 FPGA */
        s_seen_gen = g_param_gen;
        s_req      = FPGA_REQ_LOAD_PARAM;
    } else if ((uint32_t)(now - s_poll_tick) >= FPGA_APP_POLL_MS) {
        /* 低频交替回读，便于上位机核对 FPGA 侧参数与内部工作点 */
        s_poll_tick = now;
        s_poll_alt  = (uint8_t)(s_poll_alt ^ 1u);
        s_req       = (s_poll_alt != 0u) ? FPGA_REQ_READ_PARAM : FPGA_REQ_READ_DATA;
    } else {
        return;
    }

    s_req_tick     = now;
    s_req_ack_mark = g_fpga_ack_cnt;
    s_cmd_dirty    = 1u;
}

/* -------------------------------------------------------------------------- */
/* 命令帧生成                                                                  */
/* -------------------------------------------------------------------------- */

static void FpgaApp_BuildCommand(void)
{
    (void)memset(&s_cmd, 0, sizeof(s_cmd));

    /* 非实时请求：控制模式固定为"等待"（文档表2 各状态帧均如此） */
    if (s_req != FPGA_REQ_NONE) {
        s_cmd.state = FpgaApp_ReqState(s_req);
        s_cmd.mode  = FPGA_MODE_WAIT;
        if (s_req == FPGA_REQ_LOAD_PARAM) {
            FpgaApp_FillParams(&s_cmd);
        }
        FpgaLink_SetCommand(&s_cmd);
        return;
    }

    /* 正常通讯：运行指令 */
    s_cmd.state   = FPGA_ST_COMM;
    s_cmd.mode    = FpgaApp_MapMode();
    s_cmd.v_gain  = (uint16_t)g_param[PI_V_GAIN];
    s_cmd.v_limit = (uint8_t)g_param[PI_V_LIMIT];

    switch (s_cmd.mode) {
        case FPGA_MODE_OPENLOOP_V:
        case FPGA_MODE_AUTO_ZERO:
            /* 找零电压由 comm_app 写入 g_control.ref1 */
            s_cmd.uq_raw = fpga_ref_to_v_raw(g_control.ref1);
            s_cmd.ud_raw = fpga_ref_to_v_raw(g_control.ref2);
            break;

        case FPGA_MODE_CURRENT_LOOP:
            s_cmd.iq_raw = fpga_ref_to_i_raw(g_control.ref1);
            s_cmd.id_raw = fpga_ref_to_i_raw(g_control.ref2);
            break;

        default: /* FPGA_MODE_WAIT：给定保持 0 */
            break;
    }

    FpgaLink_SetCommand(&s_cmd);
}

/**
 * @brief 外部控制语义 -> FPGA 控制模式
 * @note  FPGA 停摆（无有效响应）时一律给"等待"，这是在执行层之前的一道硬门限：
 *        唯一例外是 0x88 设置参数，本实现用 0x02 加载参数表达，不使用 0x88。
 */
static uint8_t FpgaApp_MapMode(void)
{
    if (FpgaLink_Alive() == 0u) {
        return FPGA_MODE_WAIT;
    }
    if (CommApp_ZeroSearchActive() != 0u) {
        return FPGA_MODE_AUTO_ZERO;
    }
    if (CommApp_IsEnabled() == 0u) {
        return FPGA_MODE_WAIT;
    }
    return (g_control.mode == MODE_CURRENT) ? FPGA_MODE_CURRENT_LOOP : FPGA_MODE_OPENLOOP_V;
}

static uint8_t FpgaApp_ReqState(fpga_req_t req)
{
    switch (req) {
        case FPGA_REQ_LOAD_PARAM:
            return FPGA_ST_LOAD_PARAM;
        case FPGA_REQ_READ_PARAM:
            return FPGA_ST_READ_PARAM;
        case FPGA_REQ_READ_DATA:
            return FPGA_ST_READ_DATA;
        default:
            return FPGA_ST_COMM;
    }
}

/**
 * @brief 参数下装：与参数协议表 3-1 的量纲 1:1，无需换算
 * @note  0x04~0x07 的 ×16 正好等于本接口的 LSB 1/2^4；
 *        0x08 零位的 1/2^15 度与本接口表 1 一致。
 */
static void FpgaApp_FillParams(fpga_cmd_t* c)
{
    c->pole_pairs   = (uint8_t)g_param[PI_POLE_PAIRS];
    c->motor_dir    = (uint8_t)g_param[PI_MOTOR_DIR];
    c->enc_dir      = (uint8_t)g_param[PI_ENC_DIR];
    c->kp_q_raw     = (uint32_t)g_param[PI_IQ_KP] & 0x00FFFFFFu;
    c->ki_q_raw     = (uint32_t)g_param[PI_IQ_KI] & 0x00FFFFFFu;
    c->kp_d_raw     = (uint32_t)g_param[PI_ID_KP] & 0x00FFFFFFu;
    c->ki_d_raw     = (uint32_t)g_param[PI_ID_KI] & 0x00FFFFFFu;
    c->zero_pos_raw = (uint32_t)g_param[PI_ZERO_POS] & 0x00FFFFFFu;
}
