/**
 ******************************************************************************
 * @file    comm_app.c
 * @brief   应用层 · 电机控制业务语义实现
 *
 * 职责边界：
 *   1. 参数表：默认值、量程、只读项、暂存区与 bit3 锁存
 *   2. 控制字语义：使能门控、清故障 / 找零 / 锁存的上升沿判定
 *   3. 状态位：通讯超时、使能回读、保护位与故障锁存的置位与撤销
 *   4. 安全回落：下行超时或故障锁存后关输出、给定清零
 *
 * 协议原文：
 *   参数"写入的参数进入暂存区，由控制帧控制字 bit3 的上升沿统一锁存后生效"
 *   控制"bit1 至 bit3 仅在由 0 变 1 的那一帧生效"
 *       "自动找零触发时 bit0 应为 0，找零电压由设定值 1 给出，约 5 秒后自动结束"
 *       "连续 200 ms 未收到合法下行帧即关闭输出，并将 bit6 置 1，给定清零"
 ******************************************************************************
 */

#include "comm_app.h"
#include "dbg_report.h" /* 调试上报（COMM_DEBUG_REPORT=0 时全部编译为空） */
#include "nvm.h"        /* 内部 Flash 掉电保存 */
#include <string.h>

/* 掉电保存的参数范围：参数协议表 3-1 中所有"读写"项 0x01~0x0B ------------
 * 0x0C 固件版本、0x0D 协议版本是只读常量，由固件生成，不参与存储。 */
#define PARAM_STORE_BASE PI_POLE_PAIRS    /* 0x01 */
#define PARAM_STORE_LAST PI_COMM_TIMEOUT  /* 0x0B */
#define PARAM_STORE_N    (uint8_t)(PARAM_STORE_LAST - PARAM_STORE_BASE + 1u)
#define PARAM_STORE_LEN  (uint16_t)(PARAM_STORE_N * 4u) /* 11 × 4 = 44 字节 */

/* 保存节流：同一内容不重复写；两次写之间至少间隔 2 s（擦写寿命与运行抖动考虑） */
#define COMM_NVM_MIN_INTERVAL_MS 2000u

/* 全局数据区 -------------------------------------------------------------- */
volatile ctrl_up_t g_status;
volatile ctrl_down_t g_control;
int32_t g_param[PARAM_INDEX_MAX + 1];
volatile uint32_t g_param_gen;

/* 参数暂存区：写入先进这里，等控制字 bit3 上升沿统一锁存才进 g_param ---- */
static int32_t s_param_pending[PARAM_INDEX_MAX + 1];
static uint32_t s_param_mask; /* bit n = 1 表示索引 n 有未锁存的写入 */

/* 自动找零状态 ------------------------------------------------------------ */
static uint8_t s_zero_active;
static uint32_t s_zero_tick;
static int16_t s_zero_voltage;

/* 掉电保存状态 ------------------------------------------------------------ */
static uint8_t s_nvm_dirty;                    /* 有参数变化待落盘 */
static uint8_t s_nvm_loaded;                   /* 本次上电确实从 Flash 恢复了参数 */
static uint32_t s_nvm_tick;                    /* 上次写盘时刻（节流） */
static uint8_t s_nvm_snap[PARAM_STORE_LEN];    /* 已落盘内容快照，用于"内容未变不重写" */

/* 保护类状态位集合（不含使能回读/通讯超时/故障锁存） */
#define ST_FAULT_MASK                                                        \
    ((uint8_t)(ST_ANGLE_ERR | ST_OVER_CUR | ST_OVER_VOLT | ST_UNDER_VOLT |    \
               ST_OVER_TEMP))

#define PARAM_MASK_BIT(idx) ((uint32_t)1u << (uint32_t)(idx))

/* 私有函数声明 ------------------------------------------------------------ */
static void CommApp_ParamHandle(const param_frame_t* req, param_frame_t* rsp);
static uint8_t CommApp_ParamInRange(uint8_t idx, int32_t v);
static uint8_t CommApp_ParamReadOnly(uint8_t idx);
static uint8_t CommApp_IsRising(uint8_t now, uint8_t prev, uint8_t bit);
static void CommApp_ForceStop(void);
static void CommApp_StartZeroSearch(int16_t uq);
static void CommApp_ServiceZeroSearch(void);
static void CommApp_NvmRestore(void);
static void CommApp_NvmService(void);
static void CommApp_ParamsToBuf(uint8_t* buf);

/* -------------------------------------------------------------------------- */
/* 初始化与周期任务                                                            */
/* -------------------------------------------------------------------------- */

void CommApp_Init(void)
{
    (void)memset((void*)&g_status, 0, sizeof(g_status));
    (void)memset((void*)&g_control, 0, sizeof(g_control));
    (void)memset(g_param, 0, sizeof(g_param));
    (void)memset(s_param_pending, 0, sizeof(s_param_pending));
    s_param_mask = 0u;
    g_param_gen  = 0u;

    /* 出厂默认值：电压增益/限幅按母线 28 V 估算（见参数协议表 3-1 后注） */
    g_param[PI_POLE_PAIRS]   = 4;
    g_param[PI_MOTOR_DIR]    = 1;
    g_param[PI_ENC_DIR]      = 1;
    g_param[PI_IQ_KP]        = 16;
    g_param[PI_IQ_KI]        = 16;
    g_param[PI_ID_KP]        = 16;
    g_param[PI_ID_KI]        = 16;
    g_param[PI_ZERO_POS]     = 0;
    g_param[PI_V_GAIN]       = 193;
    g_param[PI_V_LIMIT]      = 16;
    g_param[PI_COMM_TIMEOUT] = (int32_t)CTRL_TIMEOUT_MS;
    g_param[PI_FW_VERSION]   = ((int32_t)FW_VER_MAJOR << 16) | FW_VER_MINOR;
    g_param[PI_PROTO_VER]    = (int32_t)PARAM_PROTO_VERSION;

    s_zero_active  = 0u;
    s_zero_tick    = 0u;
    s_zero_voltage = 0;

    s_nvm_dirty  = 0u;
    s_nvm_loaded = 0u;
    s_nvm_tick   = 0u;

    /* 从 Flash 恢复上次保存的参数（逐项过范围校验，坏数据不会生效） */
    CommApp_NvmRestore();

    /* 参数表就绪后再启动链路，避免上电首帧读到未初始化的量 */
    CommProto_Init();
}

void CommApp_Poll(void)
{
    CommProto_Poll();
    CommApp_ServiceZeroSearch();
    CommApp_NvmService();
}

uint8_t CommApp_ControlAlive(void)
{
    return CommProto_LinkAlive();
}

uint8_t CommApp_IsEnabled(void)
{
    return (uint8_t)((g_status.status & ST_ENABLED) != 0u);
}

uint8_t CommApp_ZeroSearchActive(void)
{
    return s_zero_active;
}

/* -------------------------------------------------------------------------- */
/* 协议层回调                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief  收到一帧已通过帧头/校验/ID 检查的控制帧
 * @param  c 解出的下行帧
 * @return 1 接受本帧（协议层据此刷新链路计时），0 拒收（按非法帧处理）
 */
uint8_t CommApp_OnControlFrame(const ctrl_down_t* c)
{
    uint8_t prev = g_control.ctrl;
    uint8_t ctrl;
    uint8_t enable;

    /* 1) 控制模式只能是表 3-3 定义的 1 / 2。
     *    未定义模式整帧丢弃：等价于"未收到合法下行帧"，链路计时不刷新，
     *    由 200 ms 超时保护兜底，比"猜一个模式继续跑"安全。 */
    if (CTRL_MODE_IS_VALID(c->mode) == 0u) {
        return 0u;
    }

    /* 2) bit4~7 为保留位（协议要求填 0），解析时屏蔽，不影响后续扩展 */
    ctrl = (uint8_t)(c->ctrl & CW_MASK);

    /* 3) 上升沿项：bit1~bit3 只在由 0 变 1 的那一帧生效 */
    if (CommApp_IsRising(ctrl, prev, CW_CLR_FAULT) != 0u) {
        CommApp_ClearFault();
    }
    if (CommApp_IsRising(ctrl, prev, CW_LATCH) != 0u) {
        CommApp_LatchParams();
    }
    /* 自动找零要求 bit0 为 0（文档："自动找零触发时 bit0 应为 0"），
     * 使能状态下忽略该位，避免带电找零 */
    if ((CommApp_IsRising(ctrl, prev, CW_ZERO) != 0u) && ((ctrl & CW_ENABLE) == 0u)) {
        CommApp_StartZeroSearch(c->ref1);
    }

    /* 4) 电平项：原样保存本帧控制字，作为下一帧的边沿判定基准 */
    g_control.ctrl = ctrl;
    g_control.mode = c->mode;
    g_control.ref1 = c->ref1;
    g_control.ref2 = c->ref2;

    /* 5) 收到合法下行帧即认为通讯恢复 */
    g_status.status &= (uint8_t)(~ST_COMM_LOST);

    Dbg_OnDownFrame(c); /* 调试打印（编译期可关） */

    /* 6) 自动找零期间接管给定：电压开环、Uq = 找零电压、Ud = 0、不使能 */
    if (s_zero_active != 0u) {
        g_control.mode = MODE_VOLTAGE;
        g_control.ref1 = s_zero_voltage;
        g_control.ref2 = 0;
        g_control.ctrl &= (uint8_t)(~CW_ENABLE);
        g_status.status &= (uint8_t)(~ST_ENABLED);
        /* 功率级接入点：按 g_control 输出找零电压 */
        return 1u;
    }

    /* 7) 使能门控：故障锁存期间不接受使能，须先由主控下发 bit1 清故障 */
    enable = (uint8_t)(((ctrl & CW_ENABLE) != 0u) &&
                       ((g_status.status & ST_FAULT_LATCH) == 0u));
    if (enable != 0u) {
        g_control.ctrl |= CW_ENABLE;
        g_status.status |= ST_ENABLED;
    } else {
        g_control.ctrl &= (uint8_t)(~CW_ENABLE);
        g_status.status &= (uint8_t)(~ST_ENABLED);
    }

    /* 功率级接入点：g_control 已是本帧生效值，在此驱动电流环/更新 PWM */
    return 1u;
}

/**
 * @brief  参数读写请求：索引合法性、命令码、只读属性、量程校验
 * @note   应答帧的 addr/cmd/idx 已由协议层回填，这里只给 status 与 value
 */
void CommApp_OnParamRequest(const param_frame_t* req, param_frame_t* rsp)
{
    CommApp_ParamHandle(req, rsp);
    Dbg_OnParam(req, rsp); /* 单点出口打印，避免每个分支都插一行 */
}

/**
 * @brief  参数读写的实际处理（保留早返回结构，便于按状态码分支阅读）
 */
static void CommApp_ParamHandle(const param_frame_t* req, param_frame_t* rsp)
{
    rsp->status = PARAM_ST_OK;
    rsp->value  = 0;

    if ((req->idx == 0u) || (req->idx > PARAM_INDEX_MAX)) {
        rsp->status = PARAM_ST_INDEX;
        return;
    }

    if (req->cmd == PARAM_CMD_READ) {
        /* 返回生效值：写入后尚未锁存时仍读到旧值，据此可确认锁存是否生效 */
        rsp->value = g_param[req->idx];
        return;
    }

    if (req->cmd != PARAM_CMD_WRITE) {
        rsp->status = PARAM_ST_UNKNOWN;
        return;
    }

    if (CommApp_ParamReadOnly(req->idx) != 0u) {
        rsp->status = PARAM_ST_READONLY;
        return;
    }

    if (CommApp_ParamInRange(req->idx, req->value) == 0u) {
        rsp->status = PARAM_ST_RANGE;
        return;
    }

    /* 写入暂存区，不回写生效区：生效时机由控制字 bit3 决定 */
    s_param_pending[req->idx] = req->value;
    s_param_mask |= PARAM_MASK_BIT(req->idx);
}

/**
 * @brief  上行周期到：把当前状态量交给协议层组帧
 */
void CommApp_BuildUpFrame(ctrl_up_t* up)
{
    up->status = g_status.status;
    up->angle  = g_status.angle;
    up->iq     = g_status.iq;
    up->vbus   = g_status.vbus;
}

/**
 * @brief  下行超时：关输出、给定清零、置 bit6
 * @note   超时必须同时清 ST_ENABLED，否则状态会出现 0x60（使能 + 已超时）
 *         这种自相矛盾的组合；通讯恢复后需主控重新下发 bit0 = 1。
 */
void CommApp_OnLinkTimeout(void)
{
    s_zero_active = 0u;
    CommApp_ForceStop();
    g_status.status |= ST_COMM_LOST;
    Dbg_OnEvent("LINK TIMEOUT: output off, refs cleared");
    /* 功率级接入点：输出已关断、给定已清零 */
}

/**
 * @brief  下行超时门限，取自参数表生效值
 */
uint32_t CommApp_GetLinkTimeoutMs(void)
{
    return (uint32_t)g_param[PI_COMM_TIMEOUT];
}

/* -------------------------------------------------------------------------- */
/* 状态位与参数锁存                                                            */
/* -------------------------------------------------------------------------- */

void CommApp_SetFault(uint8_t fault_bits)
{
    uint8_t bits = (uint8_t)(fault_bits & ST_FAULT_MASK);

    if (bits == 0u) {
        return;
    }

    /* 故障源位与 bit7 同时置位：主控据此判断需要下发故障清除 */
    g_status.status |= (uint8_t)(bits | ST_FAULT_LATCH);

    s_zero_active = 0u;
    CommApp_ForceStop();
    /* 功率级接入点：故障已锁存，此处必须关断 PWM */
}

void CommApp_ClearFault(void)
{
    g_status.status &= (uint8_t)(~(uint8_t)(ST_FAULT_MASK | ST_FAULT_LATCH));
    /* 只清锁存，不自动恢复输出：需主控重新下发 bit0 = 1 的下行帧。
     * 若故障条件仍在，执行层会在下一帧用 CommApp_SetFaultLevel() 把源位重新置起来，
     * 因此"清完立刻又亮"是正常现象，说明条件未消失。 */
}

/**
 * @brief  实时保护位电平更新（不锁存、不关输出）
 * @note   仅改动 fault_bit 指定的位，不影响其他来源的保护位
 */
void CommApp_SetFaultLevel(uint8_t fault_bit, uint8_t active)
{
    uint8_t bit = (uint8_t)(fault_bit & ST_FAULT_MASK);

    if (bit == 0u) {
        return;
    }
    if (active != 0u) {
        g_status.status |= bit;
    } else {
        g_status.status &= (uint8_t)(~bit);
    }
}

void CommApp_LatchParams(void)
{
    uint8_t idx;

    if (s_param_mask == 0u) {
        return;
    }

    for (idx = 1u; idx <= PARAM_INDEX_MAX; idx++) {
        if ((s_param_mask & PARAM_MASK_BIT(idx)) != 0u) {
            g_param[idx] = s_param_pending[idx];
        }
    }
    s_param_mask = 0u;

    /* 通知执行层：参数已生效，需要重新下装（见 g_param_gen 注释） */
    g_param_gen++;

    /* 标记待落盘：由 CommApp_NvmService() 在输出关闭且稳定后写入 Flash */
    s_nvm_dirty = 1u;

    /* 功率级接入点：新的 PI 增益、电压增益/限幅在此重载到调节器。
     * 通讯超时门限不必在此处理——CommApp_GetLinkTimeoutMs 每周期读生效值。 */
}

/* -------------------------------------------------------------------------- */
/* 内部实现                                                                    */
/* -------------------------------------------------------------------------- */

static uint8_t CommApp_IsRising(uint8_t now, uint8_t prev, uint8_t bit)
{
    return (uint8_t)(((now & bit) != 0u) && ((prev & bit) == 0u));
}

/**
 * @brief  强制关输出并清零给定
 */
static void CommApp_ForceStop(void)
{
    g_control.ctrl &= (uint8_t)(~CW_ENABLE);
    g_control.ref1 = 0;
    g_control.ref2 = 0;
    g_status.status &= (uint8_t)(~ST_ENABLED);
}

/**
 * @brief  启动自动找零
 * @param  uq 找零电压，取自触发帧的设定值 1
 */
static void CommApp_StartZeroSearch(int16_t uq)
{
    s_zero_active  = 1u;
    s_zero_tick    = CommProto_GetTick();
    s_zero_voltage = uq;

    g_control.ctrl &= (uint8_t)(~CW_ENABLE);
    g_status.status &= (uint8_t)(~ST_ENABLED);
    /* 功率级接入点：以 uq 作为找零电压起振，5 s 后由 ServiceZeroSearch 收尾 */
}

/**
 * @brief  自动找零计时收尾：到 5 s 自动结束，给定清零
 */
static void CommApp_ServiceZeroSearch(void)
{
    if (s_zero_active == 0u) {
        return;
    }
    if ((uint32_t)(CommProto_GetTick() - s_zero_tick) < CTRL_ZERO_SEARCH_MS) {
        return;
    }

    s_zero_active  = 0u;
    s_zero_voltage = 0;
    CommApp_ForceStop();
    /* 功率级接入点：撤掉找零电压，输出回到关断状态 */
}

/* -------------------------------------------------------------------------- */
/* 掉电保存                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 本次上电是否从 Flash 恢复了参数
 * @note  执行层据此决定要不要在启动时下装一次参数（见 fpga_app.c）
 */
uint8_t CommApp_ParamsFromNvm(void)
{
    return s_nvm_loaded;
}

/**
 * @brief 参数表序列化：小端 int32，顺序为 0x01..0x0B
 */
static void CommApp_ParamsToBuf(uint8_t* buf)
{
    uint8_t i;

    for (i = 0u; i < PARAM_STORE_N; i++) {
        uint32_t v = (uint32_t)g_param[(uint8_t)(PARAM_STORE_BASE + i)];
        uint8_t* p = &buf[(uint16_t)i * 4u];

        p[0] = (uint8_t)(v & 0xFFu);
        p[1] = (uint8_t)((v >> 8) & 0xFFu);
        p[2] = (uint8_t)((v >> 16) & 0xFFu);
        p[3] = (uint8_t)((v >> 24) & 0xFFu);
    }
}

/**
 * @brief 上电恢复：读 Flash -> 逐项范围校验 -> 写回 g_param
 * @note  校验不通过的项保留出厂默认值，因此"存储内容损坏/结构变更"不会让驱动器
 *        进入非法配置；长度与当前版本不一致时整包忽略。
 */
static void CommApp_NvmRestore(void)
{
    uint8_t buf[NVM_PAYLOAD_MAX];
    uint16_t len = 0u;
    uint8_t i;

    Nvm_Init();

    if (Nvm_Load(buf, sizeof(buf), &len) == 0u) {
        CommApp_ParamsToBuf(s_nvm_snap); /* 无记录：快照=当前默认值，避免首次锁存就写盘 */
        return;
    }
    if (len != PARAM_STORE_LEN) {
        CommApp_ParamsToBuf(s_nvm_snap);
        return;
    }

    for (i = 0u; i < PARAM_STORE_N; i++) {
        uint8_t idx = (uint8_t)(PARAM_STORE_BASE + i);
        const uint8_t* p = &buf[(uint16_t)i * 4u];
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                              ((uint32_t)p[3] << 24));

        if (CommApp_ParamInRange(idx, v) != 0u) {
            g_param[idx] = v;
        }
    }

    s_nvm_loaded = 1u;
    CommApp_ParamsToBuf(s_nvm_snap);
}

/**
 * @brief 落盘任务：仅在"有变化 + 输出关闭 + 距上次写盘 ≥2 s"时执行一次
 * @note  擦除一页约 20~40 ms，主循环会被打住，因此：
 *          1. 只在输出关闭时写，避免影响运行中的电机；
 *          2. 内容与已落盘快照相同则直接清标志，不产生擦写；
 *          3. 失败（擦除/编程/回读校验）保留脏标志，下个周期重试。
 */
static void CommApp_NvmService(void)
{
    uint8_t buf[PARAM_STORE_LEN];
    uint32_t now;

    if (s_nvm_dirty == 0u) {
        return;
    }
    if (CommApp_IsEnabled() != 0u) {
        return; /* 运行中不写 Flash：文档也建议参数在输出关闭时写入 */
    }

    now = CommProto_GetTick();
    if ((uint32_t)(now - s_nvm_tick) < COMM_NVM_MIN_INTERVAL_MS) {
        return;
    }

    CommApp_ParamsToBuf(buf);
    if (memcmp(buf, s_nvm_snap, PARAM_STORE_LEN) == 0) {
        s_nvm_dirty = 0u; /* 内容没变（例如锁存了但值相同）：不产生擦写 */
        return;
    }

    s_nvm_tick = now;

    if (Nvm_Save(buf, PARAM_STORE_LEN) != 0u) {
        (void)memcpy(s_nvm_snap, buf, PARAM_STORE_LEN);
        s_nvm_dirty = 0u;
        Dbg_OnEvent("NVM SAVE OK");
    } else {
        Dbg_OnEvent("NVM SAVE FAIL");
    }

    /* 擦写期间主循环停摆 20~40 ms，接收环形缓冲已绕回、旧数据被覆盖，
     * 必须丢弃残留并让帧状态机重新对齐，否则会拿被覆盖的字节去解帧 */
    CommProto_FlushRx();
}

/* -------------------------------------------------------------------------- */
/* 参数量程与属性                                                              */
/* -------------------------------------------------------------------------- */

static uint8_t CommApp_ParamReadOnly(uint8_t idx)
{
    return (uint8_t)((idx == PI_FW_VERSION) || (idx == PI_PROTO_VER));
}

static uint8_t CommApp_ParamInRange(uint8_t idx, int32_t v)
{
    switch (idx) {
        case PI_POLE_PAIRS:
            return (uint8_t)((v >= 1) && (v <= 100));
        case PI_MOTOR_DIR:
            return (uint8_t)((v >= 0) && (v <= 1));
        case PI_ENC_DIR:
            return (uint8_t)((v >= 0) && (v <= 1));
        case PI_IQ_KP:
        case PI_ID_KP:
            return (uint8_t)((v >= 16) && (v <= 1600));
        case PI_IQ_KI:
        case PI_ID_KI:
            return (uint8_t)((v >= 16) && (v <= 3200000));
        case PI_ZERO_POS:
            return (uint8_t)((v >= 0) && (v <= 11796480));
        case PI_V_GAIN:
            return (uint8_t)((v >= 0) && (v <= 500));
        case PI_V_LIMIT:
            return (uint8_t)((v >= 0) && (v <= 20));
        case PI_COMM_TIMEOUT:
            return (uint8_t)((v >= 10) && (v <= 1000));
        default:
            return 0u;
    }
}
