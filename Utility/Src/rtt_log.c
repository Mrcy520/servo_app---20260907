/**
 ******************************************************************************
 * @file    rtt_log.c
 * @brief   多通道日志门面实现（见 rtt_log.h 的通道分配与行原子性说明）
 *
 * 依赖关系：Utility 内部单向依赖 rtt.c，不向上依赖 App。
 * 格式化复用 rtt.c 自带的 rtt_vformat()（本就是为免 libc 设计的），
 * 不为一个日志接口去链接 newlib 的 printf——本工程 Flash 仅 60 KB，
 * 那套格式化器实测要多占 5 KB 量级。
 * 只在主循环调用，栈占用 = LOG_LINE_MAX。
 ******************************************************************************
 */

#include "rtt_log.h"

#include <string.h>

/* 通道名必须是静态存储期字符串（rtt.c 只保存指针，不拷贝） */
static const char* const k_up_name[LOG_CH_COUNT] = {
    "MCU",   /* LOG_CH_MCU   */
    "FPGA",  /* LOG_CH_FPGA  */
    "COMM",  /* LOG_CH_COMM  */
    "ANGLE"  /* LOG_CH_ANGLE */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
/* 新增通道时这里会主动报错，提醒同步 LOG_CH_COUNT 与 k_up_name[] */
_Static_assert(LOG_CH_COUNT == ((unsigned)LOG_CH_ANGLE + 1u),
               "LOG_CH_COUNT must stay in sync with log_chan_t");
_Static_assert(RTT_NUM_UP_BUFFERS >= LOG_CH_COUNT, "RTT up buffers fewer than log channels");
#endif

#define LOG_DOWN_CMD_NAME "CMD"

/* 截断行需要容纳 "..." + "\r\n" + '\0' */
#define LOG_TAIL_RESERVE 6u

/* -------------------------------------------------------------------------- */
/* 初始化                                                                      */
/* -------------------------------------------------------------------------- */

void Log_Init(void)
{
    unsigned i;

    rtt_init();

    for (i = 0u; i < LOG_CH_COUNT; i++) {
        (void)rtt_set_channel_name(i, k_up_name[i]);
        rtt_reset_dropped_bytes(i);
    }

    /* 下行通道 0 预留给"调试器下发命令"（当前工程未使用，先命名便于识别） */
    (void)rtt_set_down_channel_name(0u, LOG_DOWN_CMD_NAME);
}

/* -------------------------------------------------------------------------- */
/* 行拼装                                                                      */
/* -------------------------------------------------------------------------- */

void Log_LineInit(log_line_t* ln)
{
    if (ln == NULL) {
        return;
    }
    ln->buf[0] = '\0';
    ln->len    = 0u;
    ln->ovf    = 0u;
}

void Log_LineAdd(log_line_t* ln, const char* fmt, ...)
{
    va_list  ap;
    unsigned room;
    int      n;

    if ((ln == NULL) || (fmt == NULL) || (ln->ovf != 0u)) {
        return;
    }

    room = (unsigned)LOG_LINE_MAX - (unsigned)ln->len; /* 含结尾 '\0' 的位置 */
    if (room <= 1u) {
        ln->ovf = 1u;
        return;
    }

    va_start(ap, fmt);
    n = (int)rtt_vformat(&ln->buf[ln->len], room, fmt, ap);
    va_end(ap);

    /* rtt_vformat 保证写入量 <= room-1 且始终以 '\0' 收尾；
     * 返回值触到上限即视为截断（可能恰好填满，按溢出处理更保险）。 */
    if ((unsigned)n >= (room - 1u)) {
        ln->ovf = 1u;
    }

    ln->len = (uint16_t)((unsigned)ln->len + (unsigned)n);
}

/* 追加一个字符，越界时置溢出 */
static void Log_PutChar(log_line_t* ln, char c)
{
    if ((unsigned)ln->len + 1u < (unsigned)LOG_LINE_MAX) {
        ln->buf[ln->len++] = c;
    } else {
        ln->ovf = 1u;
    }
}

void Log_LineAddHex(log_line_t* ln, const void* data, unsigned n)
{
    static const char k_hex[] = "0123456789ABCDEF";
    const uint8_t*    p       = (const uint8_t*)data;
    unsigned          i;

    if ((ln == NULL) || (data == NULL) || (ln->ovf != 0u)) {
        return;
    }

    for (i = 0u; i < n; i++) {
        if (i != 0u) {
            Log_PutChar(ln, ' ');
        }
        Log_PutChar(ln, k_hex[(p[i] >> 4) & 0x0Fu]);
        Log_PutChar(ln, k_hex[p[i] & 0x0Fu]);
    }
}

unsigned Log_LineSend(log_line_t* ln, log_chan_t ch)
{
    if (ln == NULL) {
        return 0u;
    }

    /* 截断行统一以 "..." 收尾，避免读者误以为数据完整 */
    if (ln->ovf != 0u) {
        ln->len            = (uint16_t)((unsigned)LOG_LINE_MAX - LOG_TAIL_RESERVE);
        ln->buf[ln->len++] = '.';
        ln->buf[ln->len++] = '.';
        ln->buf[ln->len++] = '.';
    }

    if ((unsigned)ln->len + 2u < (unsigned)LOG_LINE_MAX) {
        ln->buf[ln->len++] = '\r';
        ln->buf[ln->len++] = '\n';
    } else if ((unsigned)ln->len + 1u < (unsigned)LOG_LINE_MAX) {
        ln->buf[ln->len++] = '\n';
    }

    ln->buf[ln->len] = '\0';

    return rtt_write_channel((unsigned)ch, ln->buf, (unsigned)ln->len);
}

unsigned Log_Printf(log_chan_t ch, const char* fmt, ...)
{
    log_line_t ln;
    va_list    ap;
    unsigned   room;
    int        n;

    if (fmt == NULL) {
        return 0u;
    }

    Log_LineInit(&ln);

    room = (unsigned)LOG_LINE_MAX - 3u; /* 预留 "\r\n" 与 '\0' */
    va_start(ap, fmt);
    n = (int)rtt_vformat(ln.buf, room, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return 0u;
    }
    if ((unsigned)n >= (room - 1u)) {
        ln.ovf = 1u;
    }
    ln.len = (uint16_t)n;

    return Log_LineSend(&ln, ch);
}
