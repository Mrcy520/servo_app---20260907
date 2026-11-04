/**
 ******************************************************************************
 * @file    comm_proto.c
 * @brief   协议层 · 链路/会话层实现
 *
 * 接收路径为有限状态机：DMA 空闲中断取回一批字节后逐字节喂入状态机，
 * 由首字节决定进入控制帧分支还是参数帧分支，收满定长即交付解析。
 * 两套不同长度的帧共用同一缓冲区与同一中断，并能自动从噪声中重新同步。
 *
 *   FSM_WAIT --0xAA--> FSM_CTRL  --收满10字节--> 控制帧 -> 应用层
 *   FSM_WAIT --0x5A--> FSM_PARAM_H --0xA5--> FSM_PARAM --收满16字节--> 参数帧 -> 应用层 -> 应答
 ******************************************************************************
 */

#include "comm_proto.h"
#include "comm_cfg.h" /* 调试期静默开关 COMM_DEBUG_SILENT */
#include "usart.h"
#include "main.h"

/* 通信端口映射 ------------------------------------------------------------ */
#define COMM_UART      huart1
#define COMM_UART_INST USART1
#define RX_BUF_SIZE    64u
#define TX_QUEUE_DEPTH 8u /* 1 kHz 上行帧与参数应答共用一个队列，留足余量 */
#define TX_MAX_LEN     PARAM_LEN

/* 接收状态机 -------------------------------------------------------------- */
typedef enum { FSM_WAIT = 0, FSM_CTRL, FSM_PARAM_H, FSM_PARAM } fsm_state_t;

static uint8_t s_rx_buf[RX_BUF_SIZE];
static uint16_t s_rx_pos; /* 循环 DMA 下已消费到的写指针位置 */
static uint8_t s_frame[PARAM_LEN];
static uint8_t s_idx;
static fsm_state_t s_fsm;

/* 发送队列 ---------------------------------------------------------------- */
static uint8_t s_tx_q[TX_QUEUE_DEPTH][TX_MAX_LEN];
static uint8_t s_tx_len[TX_QUEUE_DEPTH];
static uint8_t s_tx_head;
static uint8_t s_tx_count;

/* 链路状态 ---------------------------------------------------------------- */
static uint32_t s_last_ctrl_tick;
static uint32_t s_last_up_tick;
static uint8_t s_link_valid;

/* 参数应答邮箱：中断只投递，主循环负责入队 --------------------------------
 * 原因：CommProto_Send 若同时被主循环（1 kHz 上行）与接收中断（参数应答）调用，
 * 即使加临界区也存在"队列 slot 长度与内容错配"的风险（实测线上偶发 6 个 0x00，
 * 即 16 字节传输里装着 10 字节上行帧的尾巴）。改为单生产者后，Send 只有主循环
 * 一个调用者，该类问题从结构上消失。一问一答场景下最多只有一个应答待发。 */
static param_frame_t s_rsp_mailbox;
static volatile uint8_t s_rsp_pending;

/* 协议层统计量 ------------------------------------------------------------ */
volatile uint32_t g_stat_ctrl_err;
volatile uint32_t g_stat_param_err;
volatile uint32_t g_stat_timeout;
volatile uint32_t g_stat_rx_err;
volatile uint32_t g_stat_tx_drop;

/* 私有函数声明 ------------------------------------------------------------ */
static void CommProto_RxByte(uint8_t b);
static uint8_t CommProto_OnCtrlFrame(void);
static void CommProto_OnParamFrame(void);
static void CommProto_TxKick(void);
static void CommProto_StartRx(void);

#if (COMM_DEBUG_SILENT == 0)
static void CommProto_SendUp(void);
static void CommProto_ServiceParamRsp(void);
#endif

/* -------------------------------------------------------------------------- */
/* 初始化与周期任务                                                            */
/* -------------------------------------------------------------------------- */

void CommProto_Init(void)
{
    uint8_t i;

    s_fsm            = FSM_WAIT;
    s_idx            = 0u;
    s_tx_head        = 0u;
    s_tx_count       = 0u;
    s_last_ctrl_tick = 0u;
    s_last_up_tick   = 0u;
    s_link_valid     = 0u;
    s_rsp_pending    = 0u;

    g_stat_ctrl_err  = 0u;
    g_stat_param_err = 0u;
    g_stat_timeout   = 0u;
    g_stat_rx_err    = 0u;
    g_stat_tx_drop   = 0u;

    for (i = 0u; i < RX_BUF_SIZE; i++) {
        s_rx_buf[i] = 0u;
    }
    s_rx_pos = 0u;

    /* 关键：接收 DMA 改为循环模式，彻底消除"每次 IDLE 后重装接收"的盲区。
     * 实测（2026-11-04）：普通模式下，紧跟在控制帧后面的参数帧有 36% 被丢掉
     * （重装窗口约 20~50 us，正好吃掉 16 字节参数帧的帧头）；
     * 1 kHz 控制流下参数读丢应答约 13%，0 Hz 时为 0%。
     * 循环模式下 HAL 不停接收、不关 IDLE（stm32f3xx_hal_uart.c:2300），
     * 回调给出的 Size 就是 DMA 当前写指针，按位置增量取数即可，不再有窗口。 */
    if (COMM_UART.hdmarx != NULL) {
        COMM_UART.hdmarx->Init.Mode = DMA_CIRCULAR;
        (void)HAL_DMA_Init(COMM_UART.hdmarx);
    }

    CommProto_StartRx();
}

void CommProto_Poll(void)
{
    uint32_t now = HAL_GetTick();

#if (COMM_DEBUG_SILENT == 0)
    /* 参数应答优先：一问一答，尽量缩短应答延迟 */
    CommProto_ServiceParamRsp();

    /* 上行按固定周期发送；调试期静默时不发，串口留给文本日志 */
    if ((uint32_t)(now - s_last_up_tick) >= COMM_UP_PERIOD_MS) {
        s_last_up_tick = now;
        CommProto_SendUp();
    }
#endif

    /* 下行超时判定：只负责计时与上报，如何回落由应用层决定 */
    if (s_link_valid != 0u) {
        uint32_t timeout = CommApp_GetLinkTimeoutMs();

        if ((timeout < 10u) || (timeout > 1000u)) {
            timeout = CTRL_TIMEOUT_MS;
        }
        if ((uint32_t)(now - s_last_ctrl_tick) > timeout) {
            s_link_valid = 0u;
            g_stat_timeout++;
            CommApp_OnLinkTimeout();
        }
    }
}

uint8_t CommProto_LinkAlive(void)
{
    return s_link_valid;
}

uint32_t CommProto_GetTick(void)
{
    return HAL_GetTick();
}

void CommProto_FlushRx(void)
{
    uint16_t pos = 0u;
    uint32_t primask;

    primask = __get_PRIMASK();
    __disable_irq();

    if (COMM_UART.hdmarx != NULL) {
        /* 循环模式下"已收字节数" = 缓冲区长度 - DMA 剩余计数 */
        pos = (uint16_t)(RX_BUF_SIZE - (uint16_t)__HAL_DMA_GET_COUNTER(COMM_UART.hdmarx));
        if (pos > RX_BUF_SIZE) {
            pos = 0u;
        }
    }
    s_rx_pos = (pos >= RX_BUF_SIZE) ? 0u : pos;
    s_fsm    = FSM_WAIT;
    s_idx    = 0u;

    if (primask == 0u) {
        __enable_irq();
    }
}

/* -------------------------------------------------------------------------- */
/* 接收                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief  USART 接收事件（IDLE / DMA 半满 / DMA 满）：按写指针增量取数
 * @param  Size 循环 DMA 下为"已接收字节数"，即当前写指针位置
 * @note   循环模式下 HAL 不会停止接收，因此本回调不再重装 DMA，
 *         从根本上消除了普通模式"IDLE 后重装"期间丢字节的问题。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size)
{
    uint16_t pos = Size;
    uint16_t i;

    if (huart->Instance != COMM_UART_INST) {
        return;
    }

#if (COMM_DIAG_ECHO != 0)
    if (pos > 0u) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        (void)HAL_UART_Transmit(&COMM_UART, s_rx_buf, pos, 20u);
    }
    return;
#else
    if (pos > RX_BUF_SIZE) {
        pos = RX_BUF_SIZE; /* 防御：异常值按满缓冲处理 */
    }
    if (pos == s_rx_pos) {
        return; /* 无新数据 */
    }

    if (pos > s_rx_pos) {
        for (i = s_rx_pos; i < pos; i++) {
            CommProto_RxByte(s_rx_buf[i]);
        }
    } else {
        /* 指针已回绕：先取尾部，再取头部 */
        for (i = s_rx_pos; i < RX_BUF_SIZE; i++) {
            CommProto_RxByte(s_rx_buf[i]);
        }
        for (i = 0u; i < pos; i++) {
            CommProto_RxByte(s_rx_buf[i]);
        }
    }

    /* DMA 满（Size == RX_BUF_SIZE）时指针回到 0，直接归零以便下轮比较 */
    s_rx_pos = (pos >= RX_BUF_SIZE) ? 0u : pos;
#endif
}

/**
 * @brief  重新装载接收；若句柄仍被占用则先中止再装载
 */
static void CommProto_StartRx(void)
{
    if (HAL_UARTEx_ReceiveToIdle_DMA(&COMM_UART, s_rx_buf, (uint16_t)RX_BUF_SIZE) != HAL_OK) {
        (void)HAL_UART_AbortReceive(&COMM_UART);
        (void)HAL_UARTEx_ReceiveToIdle_DMA(&COMM_UART, s_rx_buf, (uint16_t)RX_BUF_SIZE);
    }
}

/**
 * @brief  接收错误：清标志、复位状态机、重新装载
 * @note   溢出或帧错误会让 HAL 终止接收，不恢复则接收永久失效
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
    if (huart->Instance != COMM_UART_INST) {
        return;
    }

    g_stat_rx_err++;
    __HAL_UART_CLEAR_OREFLAG(huart);

    s_fsm = FSM_WAIT;
    s_idx = 0u;

    if (huart->RxState != HAL_UART_STATE_READY) {
        (void)HAL_UART_AbortReceive(huart);
    }

    /* 重装后 DMA 从 0 开始写，写指针基准同步归零 */
    s_rx_pos = 0u;
    (void)memset(s_rx_buf, 0, sizeof(s_rx_buf));

    CommProto_StartRx();
}

/**
 * @brief  状态机单字节推进
 */
static void CommProto_RxByte(uint8_t b)
{
    switch (s_fsm) {
        case FSM_WAIT:
            if (b == CTRL_HEAD) {
                s_frame[0] = b;
                s_idx      = 1u;
                s_fsm      = FSM_CTRL;
            } else if (b == PARAM_HEAD0) {
                s_frame[0] = b;
                s_idx      = 1u;
                s_fsm      = FSM_PARAM_H;
            }
            break;

        case FSM_PARAM_H:
            if (b == PARAM_HEAD1) {
                s_frame[1] = b;
                s_idx      = 2u;
                s_fsm      = FSM_PARAM;
            } else if (b == PARAM_HEAD0) {
                s_idx = 1u;
            } else {
                s_fsm = FSM_WAIT;
            }
            break;

        case FSM_CTRL:
            s_frame[s_idx++] = b;
            if (s_idx >= CTRL_DOWN_LEN) {
                uint8_t ok = CommProto_OnCtrlFrame();

                s_fsm = FSM_WAIT;
                s_idx = 0u;
                /* 本帧被拒收时，末字节可能是下一帧的帧头，就地重新对齐，
                 * 避免因一帧错位连续丢失后续帧 */
                if (ok == 0u) {
                    CommProto_RxByte(b);
                }
            }
            break;

        case FSM_PARAM:
            s_frame[s_idx++] = b;
            if (s_idx >= PARAM_LEN) {
                CommProto_OnParamFrame();
                s_fsm = FSM_WAIT;
                s_idx = 0u;
            }
            break;

        default:
            s_fsm = FSM_WAIT;
            s_idx = 0u;
            break;
    }
}

/* -------------------------------------------------------------------------- */
/* 帧处理                                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief  控制帧到齐：校验后交应用层判定，接受则刷新链路计时
 * @return 1 帧被接受（链路计时已刷新），0 帧被拒收
 * @note   文档要求"连续 200 ms 未收到合法下行帧即关闭输出"，因此链路计时
 *         必须放在应用层判定之后：业务上非法的帧不能维持链路有效。
 */
static uint8_t CommProto_OnCtrlFrame(void)
{
    ctrl_down_t c;

    if (ctrl_unpack_down(s_frame, &c) != 0) {
        g_stat_ctrl_err++;
        return 0u;
    }

    if (CommApp_OnControlFrame(&c) == 0u) {
        g_stat_ctrl_err++;
        return 0u;
    }

    s_last_ctrl_tick = HAL_GetTick();
    s_link_valid     = 1u;
    return 1u;
}

/**
 * @brief  参数帧到齐：校验、寻址过滤后交应用层应答
 */
static void CommProto_OnParamFrame(void)
{
    param_frame_t req;
    param_frame_t rsp;

    if (param_unpack(s_frame, &req) != 0) {
        g_stat_param_err++;
        return;
    }
    if (req.addr != COMM_PROTO_ADDR) {
        return; /* 非本站，静默丢弃 */
    }

    /* 应答帧的寻址与命令码原样回显，应用层只填 status 与 value */
    memset(&rsp, 0, sizeof(rsp));
    rsp.addr = req.addr;
    rsp.cmd  = req.cmd;
    rsp.idx  = req.idx;

    CommApp_OnParamRequest(&req, &rsp);
#if (COMM_DEBUG_SILENT == 0)
    /* 交给主循环发送：中断里不入队，避免与上行帧争同一队列索引 */
    s_rsp_mailbox = rsp;
    s_rsp_pending = 1u;
#endif
}

/* -------------------------------------------------------------------------- */
/* 发送                                                                        */
/* -------------------------------------------------------------------------- */

#if (COMM_DEBUG_SILENT == 0)
static void CommProto_SendUp(void)
{
    uint8_t buf[CTRL_UP_LEN];
    ctrl_up_t up;

    memset(&up, 0, sizeof(up));
    CommApp_BuildUpFrame(&up);

    ctrl_pack_up(buf, &up);
    (void)CommProto_Send(buf, CTRL_UP_LEN);
}

static void CommProto_ServiceParamRsp(void)
{
    param_frame_t rsp;
    uint8_t buf[PARAM_LEN];
    uint32_t primask;

    if (s_rsp_pending == 0u) {
        return;
    }

    /* 与中断侧的投递互斥；临界区只有结构体拷贝，极短 */
    primask = __get_PRIMASK();
    __disable_irq();
    rsp           = s_rsp_mailbox;
    s_rsp_pending = 0u;
    if (primask == 0u) {
        __enable_irq();
    }

    param_pack(buf, &rsp);
    (void)CommProto_Send(buf, PARAM_LEN);
}
#endif /* COMM_DEBUG_SILENT == 0 */

uint8_t CommProto_Send(const uint8_t* buf, uint8_t len)
{
    uint8_t slot;
    uint32_t primask;

    if (len > TX_MAX_LEN) {
        g_stat_tx_drop++;
        return 0u;
    }

    /* 本函数被两处调用：主循环（1 kHz 上行帧）与 USART 接收中断（参数应答）。
     * slot 计算 + s_tx_count 自增必须是原子的，否则两路会算出同一个 slot，
     * 后写的一路直接覆盖前一路的帧——实测表现为约 2.5% 的参数应答凭空消失，
     * 而线上其余帧完全正确（用原始字节离线分析确认过：4009 帧控制 + 195 帧应答，0 异常）。 */
    primask = __get_PRIMASK();
    __disable_irq();

    if (s_tx_count >= TX_QUEUE_DEPTH) {
        if (primask == 0u) {
            __enable_irq();
        }
        g_stat_tx_drop++;
        return 0u;
    }

    slot = (uint8_t)((s_tx_head + s_tx_count) % TX_QUEUE_DEPTH);
    memcpy(s_tx_q[slot], buf, len);
    s_tx_len[slot] = len;
    s_tx_count++;

    if (primask == 0u) {
        __enable_irq();
    }

    CommProto_TxKick(); /* 放到临界区外：HAL 启动 DMA 不宜长时间关中断 */
    return 1u;
}

static void CommProto_TxKick(void)
{
    if (s_tx_count == 0u) {
        return;
    }
    if (COMM_UART.gState != HAL_UART_STATE_READY) {
        return;
    }
    (void)HAL_UART_Transmit_DMA(&COMM_UART, s_tx_q[s_tx_head], (uint16_t)s_tx_len[s_tx_head]);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart)
{
    uint32_t primask;

    if (huart->Instance != COMM_UART_INST) {
        return;
    }

    /* 与 CommProto_Send 的入队临界区对称：避免更高优先级中断在改 head/count 的中途
     * 插入一次入队，导致队列索引失配 */
    primask = __get_PRIMASK();
    __disable_irq();
    if (s_tx_count > 0u) {
        s_tx_head = (uint8_t)((s_tx_head + 1u) % TX_QUEUE_DEPTH);
        s_tx_count--;
    }
    if (primask == 0u) {
        __enable_irq();
    }

    CommProto_TxKick();
}
