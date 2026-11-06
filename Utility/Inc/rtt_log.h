/**
 ******************************************************************************
 * @file    rtt_log.h
 * @brief   多通道日志门面 · 把不同来源的信息分流到不同 RTT 上行通道
 *
 * 为什么需要门面：
 *   RTT 的每条上行通道在调试器里就是**一个独立终端**。把"谁产生的信息"
 *   与"在哪看"解耦之后，FPGA 16 kHz 数据流、外部协议 1 kHz 报文、
 *   MCU 自身状态各占一个窗口，互不刷屏，也不需要靠前缀标签去人眼过滤。
 *
 * 通道分配（默认 4 条上行通道；改 rtt.h 的 RTT_NUM_UP_BUFFERS 可扩展）：
 *   LOG_CH_MCU   ch0  MCU 自身：启动信息 / 状态位变化 / 参数读写 /
 *                    链路超时 / NVM / 1 Hz 统计 / printf 兜底
 *   LOG_CH_FPGA  ch1  FPGA 内部接口：0x01 实时帧解码值 / SPI3 链路统计
 *   LOG_CH_COMM  ch2  外部协议：下行帧、上行帧（含线上 hex，便于对照抓包）
 *   LOG_CH_ANGLE ch3  角度单值流：每行只有 "angle:359.997"，无时间戳无其它字段，
 *                    一行一个样本、等间隔输出，主机侧可直接按 x/y 作图
 *
 * 行原子性（重要）：
 *   rtt.c 的缓冲区 Flags=0（NO_BLOCK_SKIP），空间不足时**整条丢弃**。
 *   因此不能像 printf 那样把一行拆成多次写入——那样会出现"前半行进来了、
 *   后半行被丢掉"的半行垃圾。本模块用 log_line_t 在本地栈上拼好整行，
 *   Log_LineSend() 一次性提交：要么整行出现，要么整条丢弃。
 *   丢弃字节数用 rtt_get_dropped_bytes(LOG_CH_xxx) 查询。
 *
 * 与协议口的关系：
 *   RTT 走调试探针的 SWD/JTAG，**不占用任何 UART**。所以本工程接入 RTT 后，
 *   USART1 可以完全留给协议（COMM_DEBUG_SILENT 不再必须为 1）。
 ******************************************************************************
 */

#ifndef __RTT_LOG_H
#define __RTT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "rtt.h"

/* ===== 上行通道分配 ====================================================== */
/* 通道号用枚举保证类型安全；通道数另用宏，因为预处理器 #if 里枚举会被当成 0，
 * 那样下面的 #error 校验会静默失效。两者必须同步（见 rtt_log.c 的静态断言）。 */
typedef enum {
    LOG_CH_MCU   = 0, /* MCU 自身信息 */
    LOG_CH_FPGA  = 1, /* FPGA 内部接口数据 */
    LOG_CH_COMM  = 2, /* 外部协议报文 */
    LOG_CH_ANGLE = 3  /* 角度单值流：每行一个样本，供主机侧作图 */
} log_chan_t;

#define LOG_CH_COUNT 4u

#if (RTT_NUM_UP_BUFFERS < LOG_CH_COUNT)
#error "RTT_NUM_UP_BUFFERS is too small: raise it to at least LOG_CH_COUNT"
#endif

/* 单行最大长度（含结尾 '\0'）。一行约 5~7 次 Dbg 追加，192 足够；
 * 栈上分配，本工程只在主循环调用，不需要为中断上下文压缩。 */
#ifndef LOG_LINE_MAX
#define LOG_LINE_MAX 192u
#endif

#if (LOG_LINE_MAX < 32u)
#error "LOG_LINE_MAX must be >= 32"
#endif

/* 行拼装缓冲 */
typedef struct {
    char     buf[LOG_LINE_MAX];
    uint16_t len; /* 已写字符数（不含结尾 '\0'） */
    uint8_t  ovf; /* 1 = 发生过截断，本行尾部以 "..." 结尾 */
} log_line_t;

/**
 * @brief  初始化 RTT 控制块并给各通道命名
 * @note   必须在第一次日志输出前调用（rtt.c 内部也会懒初始化，
 *         但那时通道名还是默认的 "Terminal"，调试器里不好分辨）。
 */
void Log_Init(void);

/**
 * @brief  开始拼装一行
 */
void Log_LineInit(log_line_t* ln);

/**
 * @brief  向当前行追加格式化文本（printf 语义，支持 %d/%u/%x/%s/%f 等）
 * @note   超长会截断并置 ovf，不会越界；已截断的行不再接受追加。
 */
void Log_LineAdd(log_line_t* ln, const char* fmt, ...);

/**
 * @brief  向当前行追加十六进制字节串（大写、空格分隔，如 "AA 55 01"）
 */
void Log_LineAddHex(log_line_t* ln, const void* data, unsigned n);

/**
 * @brief  把整行提交到指定通道（自动补 "\r\n"）
 * @retval 实际写入字节数；0 表示整条被丢弃（缓冲区不足）
 */
unsigned Log_LineSend(log_line_t* ln, log_chan_t ch);

/**
 * @brief  一次性输出一整行（内部完成拼装与提交）
 * @retval 实际写入字节数
 */
unsigned Log_Printf(log_chan_t ch, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __RTT_LOG_H */
