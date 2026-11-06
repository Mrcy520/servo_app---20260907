/**
 * @file    rtt.c
 * @brief   RTT（Real Time Transfer）单文件移植库
 *
 * 与 SEGGER RTT 协议兼容：控制块布局、缓冲区描述符字段顺序与官方实现一致，
 * 主机（本工具 / SEGGER RTT Viewer / OpenOCD RTT）在 RAM 中搜索 "SEGGER RTT"
 * 字符串即可定位控制块，随后读取各通道环形缓冲区。
 *
 * 设计要点：
 *   1. 只实现直接内存访问模式（通道 0 默认方式），非阻塞；
 *   2. Flags 置 0（NO_BLOCK_SKIP），缓冲区放不下时整条丢弃，不会写出半行；
 *   3. 丢弃字节数可查询，便于判断“没打印”还是“丢了”；
 *   4. 格式化在锁外完成，仅在写环形缓冲区时关中断，缩短中断屏蔽时间；
 *   5. 初始化关中断 + 双重检查，任务/中断同时首次调用不会互相破坏；
 *   6. 对调试器写入的 WrOff/RdOff 做合法性校正，避免越界读写；
 *   7. 无 .S、无 HAL/RTOS/CMSIS、无 libc 依赖，控制块为普通全局变量。
 */

#include "rtt.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ICCARM__)
#include <intrinsics.h>
#endif

/* ======================== 协议数据结构 ======================== */

/* 上行缓冲区描述符（目标 -> 主机），字段顺序与 SEGGER 协议一致 */
typedef struct {
    const char* sName;          /* 通道名 */
    char* pBuffer;              /* 环形缓冲区首地址 */
    uint32_t SizeOfBuffer;      /* 缓冲区大小（字节） */
    volatile uint32_t WrOff;    /* 目标写入位置 */
    volatile uint32_t RdOff;    /* 主机读取位置（调试器会修改） */
    uint32_t Flags;             /* 0 = NO_BLOCK_SKIP（写不下则整条丢弃） */
} rtt_up_buffer_t;

/* 下行缓冲区描述符（主机 -> 目标） */
typedef struct {
    const char* sName;
    char* pBuffer;
    uint32_t SizeOfBuffer;
    volatile uint32_t WrOff;    /* 主机写入位置（调试器会修改） */
    volatile uint32_t RdOff;    /* 目标读取位置 */
    uint32_t Flags;
} rtt_down_buffer_t;

/* RTT 控制块，布局与 SEGGER 协议严格一致 */
typedef struct {
    char acID[16];              /* 固定为 "SEGGER RTT" */
    int MaxNumUpBuffers;
    int MaxNumDownBuffers;
    rtt_up_buffer_t aUp[RTT_NUM_UP_BUFFERS];
    rtt_down_buffer_t aDown[RTT_NUM_DOWN_BUFFERS];
} rtt_control_block_t;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#if defined(__arm__) || defined(__ARM_ARCH) || defined(_M_ARM)
_Static_assert(sizeof(rtt_up_buffer_t) == 24u, "RTT up buffer layout must be 24 bytes");
_Static_assert(sizeof(rtt_down_buffer_t) == 24u, "RTT down buffer layout must be 24 bytes");
_Static_assert(offsetof(rtt_control_block_t, MaxNumUpBuffers) == 16u,
               "RTT control block header layout mismatch");
_Static_assert(offsetof(rtt_control_block_t, aUp) == 24u,
               "RTT control block up buffers offset mismatch");
#endif
#endif

/* 控制块必须是全局变量：主机靠在 RAM 中搜索该结构内的标识串来定位 */
rtt_control_block_t rtt_control_block;

static char s_up_buffers[RTT_NUM_UP_BUFFERS][RTT_BUFFER_SIZE_UP];
static char s_down_buffers[RTT_NUM_DOWN_BUFFERS][RTT_BUFFER_SIZE_DOWN];

/* 所有通道默认同名，可用 rtt_set_channel_name / rtt_set_down_channel_name 修改 */
static const char s_channel_name[] = "Terminal";

static volatile unsigned s_initialized = 0u;
static volatile uint32_t s_dropped[RTT_NUM_UP_BUFFERS];

/* ======================== 临界区与内存屏障 ======================== */

#if defined(__ICCARM__)

typedef __istate_t rtt_state_t;

static rtt_state_t rtt_lock(void)
{
    rtt_state_t state = __get_interrupt_state();
    __disable_interrupt();
    return state;
}

static void rtt_unlock(rtt_state_t state)
{
    __set_interrupt_state(state);
}

static void rtt_memory_barrier(void)
{
    __DMB();
}

#elif defined(__CC_ARM)

/* ARM Compiler 5：内嵌汇编读写 PRIMASK，另用编译器屏障 + DMB 保证写顺序 */
typedef unsigned rtt_state_t;

__asm static unsigned rtt_lock_state(void)
{
    MRS     r0, PRIMASK
    CPSID   i
    BX      lr
}

__asm static void rtt_restore_state(unsigned state)
{
    MSR     PRIMASK, r0
    BX      lr
}

static rtt_state_t rtt_lock(void)
{
    return (rtt_state_t)rtt_lock_state();
}

static void rtt_unlock(rtt_state_t state)
{
    rtt_restore_state((unsigned)state);
}

static void rtt_memory_barrier(void)
{
    __schedule_barrier(); /* 编译器屏障：数据写入不能重排到 WrOff 更新之后 */
    __dmb(0xF);           /* DMB SY */
}

#elif defined(__GNUC__) || defined(__clang__)

typedef unsigned rtt_state_t;

static rtt_state_t rtt_lock(void)
{
    unsigned state = 0u;

#if defined(__arm__) || defined(__ARM_ARCH)
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(state) : : "memory");
#endif

    return state;
}

static void rtt_unlock(rtt_state_t state)
{
#if defined(__arm__) || defined(__ARM_ARCH)
    /* 原来未关中断才恢复；原本就关中断则保持关闭 */
    if ((state & 1u) == 0u) {
        __asm volatile("cpsie i" : : : "memory");
    }
#else
    (void)state;
#endif
}

static void rtt_memory_barrier(void)
{
#if defined(__arm__) || defined(__ARM_ARCH)
#if defined(__ARM_ARCH_6M__) || (defined(__ARM_ARCH) && (__ARM_ARCH == 6))
    __asm volatile("dmb 0xf" : : : "memory");
#else
    __asm volatile("dmb sy" : : : "memory");
#endif
#endif
}

#elif defined(RTT_NO_CRITICAL_SECTION)

/* 用户显式关闭临界区保护（仅建议用于前后台单任务且无中断竞争的场景） */
typedef unsigned rtt_state_t;

static rtt_state_t rtt_lock(void)
{
    return 0u;
}

static void rtt_unlock(rtt_state_t state)
{
    (void)state;
}

static void rtt_memory_barrier(void)
{
}

#else
/*
 * 未知编译器：不再静默降级，避免多任务/中断环境下丢数据却无人察觉。
 * 如需支持，请在此处补充关中断实现，或在工程宏中定义 RTT_NO_CRITICAL_SECTION
 * 明确接受无保护运行。
 */
#error "Unsupported compiler for RTT critical section. Define RTT_NO_CRITICAL_SECTION to opt out."
#endif

/* ======================== 最小 libc 替代 ======================== */

/*
 * 这里原本是逐字节 + volatile 的手写循环（为了不引入 libc）。实测在本工程
 * 16 kHz SPI 同步的场景下有问题：写环形缓冲时是**关中断**的，逐字节 volatile
 * 在 -O0 下约 10 个周期/字节，一行 150 字节就要屏蔽中断 ~20 µs；Dbg_Poll 一次
 * 连写 [FP]/[ANG]/[RAW] 三行可达 ~50 µs，直接把 62.5 µs 周期里的 EXTI 推迟到
 * 下一个周期，造成 ovr 增长（现场实测同步沿间隔在 14~131 µs 之间抖动）。
 *
 * 改用 libc 的 memcpy/memset：同样是"不依赖 HAL/RTOS"，但内部按字搬运，
 * 512 字节上限的关中断时间从 ~40 µs 降到 ~5 µs。工程本就在用 string.h。
 */
#define RTT_USE_LIBC_MEM 1

static void rtt_mem_set(void* dst, unsigned char value, unsigned len)
{
#if RTT_USE_LIBC_MEM
    (void)memset(dst, (int)value, (size_t)len);
#else
    volatile unsigned char* p = (volatile unsigned char*)dst;
    while (len != 0u) {
        *p++ = value;
        len--;
    }
#endif
}

static void rtt_mem_copy(void* dst, const void* src, unsigned len)
{
#if RTT_USE_LIBC_MEM
    (void)memcpy(dst, src, (size_t)len);
#else
    volatile unsigned char* d       = (volatile unsigned char*)dst;
    const volatile unsigned char* s = (const volatile unsigned char*)src;
    while (len != 0u) {
        *d++ = *s++;
        len--;
    }
#endif
}

static unsigned rtt_str_len(const char* s)
{
#if RTT_USE_LIBC_MEM
    return (unsigned)strlen(s);
#else
    const volatile char* p = s;
    unsigned n = 0u;
    while (p[n] != '\0') {
        n++;
    }
    return n;
#endif
}

#if RTT_PRINTF_SUPPORT_FLOAT
/* 把 src 追加到 body，使用 volatile 目标避免被优化成 memcpy 调用 */
static void rtt_append(char* body, unsigned* pos, const char* src, unsigned n)
{
    volatile char* d = &body[*pos];
    unsigned i;
    for (i = 0u; i < n; i++) {
        d[i] = src[i];
    }
    *pos += n;
}
#endif

/* ======================== 格式化输出 ======================== */

typedef struct {
    char* buf;
    unsigned cap;   /* 缓冲区总容量（含结尾 '\0' 的位置） */
    unsigned len;   /* 已写入字符数 */
} rtt_writer_t;

static void rtt_w_char(rtt_writer_t* w, char c)
{
    if (w->len < (w->cap - 1u)) {
        w->buf[w->len++] = c;
    }
}

/* 按宽度输出一段文本，支持左对齐 */
static void rtt_w_text(rtt_writer_t* w, const char* text, unsigned len,
                       unsigned width, int has_width, int left)
{
    unsigned pad = (has_width && (width > len)) ? (width - len) : 0u;
    unsigned i;

    if (!left) {
        for (i = 0u; i < pad; i++) {
            rtt_w_char(w, ' ');
        }
    }
    for (i = 0u; i < len; i++) {
        rtt_w_char(w, text[i]);
    }
    if (left) {
        for (i = 0u; i < pad; i++) {
            rtt_w_char(w, ' ');
        }
    }
}

/* 按宽度输出已拼好的数字体（处理符号/前缀之后的补零与左对齐） */
static void rtt_w_padded(rtt_writer_t* w, const char* body, unsigned len,
                         unsigned width, int has_width, int left, int zero,
                         unsigned prefix_len)
{
    unsigned pad = (has_width && (width > len)) ? (width - len) : 0u;
    unsigned i;

    if (left) {
        for (i = 0u; i < len; i++) {
            rtt_w_char(w, body[i]);
        }
        for (i = 0u; i < pad; i++) {
            rtt_w_char(w, ' ');
        }
    } else if (zero && (pad != 0u)) {
        /* 0 填充放在符号/0x 前缀之后 */
        for (i = 0u; i < prefix_len && i < len; i++) {
            rtt_w_char(w, body[i]);
        }
        for (i = 0u; i < pad; i++) {
            rtt_w_char(w, '0');
        }
        for (i = prefix_len; i < len; i++) {
            rtt_w_char(w, body[i]);
        }
    } else {
        for (i = 0u; i < pad; i++) {
            rtt_w_char(w, ' ');
        }
        for (i = 0u; i < len; i++) {
            rtt_w_char(w, body[i]);
        }
    }
}

/* 整数格式化（符号、宽度、精度、0 填充、左对齐、# 前缀） */
static void rtt_w_int(rtt_writer_t* w, unsigned value, int negative,
                      unsigned base, int upper,
                      unsigned width, int has_width,
                      unsigned prec, int has_prec,
                      int left, int zero, int plus, int space_flag, int hash)
{
    char body[48];
    char digits[12];
    unsigned orig = value;
    unsigned nd = 0u;
    unsigned bl = 0u;
    unsigned prec_zeros;
    unsigned prefix_len;
    unsigned i;

    if ((value == 0u) && has_prec && (prec == 0u)) {
        nd = 0u; /* "%.0d" 且值为 0：不输出数字 */
    } else {
        do {
            unsigned d = value % base;
            digits[nd++] = (char)((d < 10u) ? ('0' + d)
                                            : ((upper ? 'A' : 'a') + d - 10u));
            value /= base;
        } while ((value != 0u) && (nd < (unsigned)sizeof(digits)));
    }

    prec_zeros = (has_prec && (prec > nd)) ? (prec - nd) : 0u;

    if (negative) {
        body[bl++] = '-';
    } else if (plus) {
        body[bl++] = '+';
    } else if (space_flag) {
        body[bl++] = ' ';
    }

    if (hash && (orig != 0u)) {
        if (base == 16u) {
            body[bl++] = '0';
            body[bl++] = upper ? 'X' : 'x';
        } else if ((base == 8u) && (prec_zeros == 0u) &&
                   (nd != 0u) && (digits[nd - 1u] != '0')) {
            body[bl++] = '0';
        }
    }

    prefix_len = bl;

    for (i = 0u; i < prec_zeros; i++) {
        body[bl++] = '0';
    }
    for (i = nd; i > 0u; i--) {
        body[bl++] = digits[i - 1u];
    }

    /* 指定精度时按标准忽略 0 填充 */
    rtt_w_padded(w, body, bl, width, has_width, left,
                 (zero && !has_prec), prefix_len);
}

#if RTT_PRINTF_SUPPORT_FLOAT

/* uint64 转十进制字符串（除以常数 10，编译器会优化为乘法移位） */
static unsigned rtt_u64_digits(uint64_t value, char* out)
{
    char tmp[20];
    unsigned n = 0u;
    unsigned i;

    do {
        tmp[n++] = (char)('0' + (unsigned)(value % 10u));
        value /= 10u;
    } while ((value != 0u) && (n < (unsigned)sizeof(tmp)));

    for (i = 0u; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }

    return n;
}

/* 浮点数格式化（定点输出，支持宽度/精度/符号/0 填充/左对齐） */
static void rtt_w_float(rtt_writer_t* w, double value,
                        unsigned width, int has_width,
                        unsigned prec, int has_prec,
                        int left, int zero, int plus, int space_flag)
{
    char body[64];
    char ip_digits[24];
    char frac_digits[12];
    unsigned bl = 0u;
    unsigned nd;
    unsigned i;
    unsigned prefix_len;
    int negative = 0;
    uint64_t ip;
    uint64_t frac;
    uint64_t scale = 1u;
    double frac_part;

    /* NaN：value != value */
    if (value != value) {
        rtt_w_text(w, "nan", 3u, width, has_width, left);
        return;
    }
    if (value > 1.7976931348623157e308) {
        rtt_w_text(w, "inf", 3u, width, has_width, left);
        return;
    }
    if (value < -1.7976931348623157e308) {
        rtt_w_text(w, "-inf", 4u, width, has_width, left);
        return;
    }

    if (value < 0.0) {
        negative = 1;
        value = -value;
    }

    if (!has_prec) {
        prec = 6u; /* 默认 6 位小数 */
    }
    if (prec > 9u) {
        prec = 9u;
    }

    if (value >= 18446744073709551616.0) {
        rtt_w_text(w, negative ? "-inf" : "inf", negative ? 4u : 3u,
                   width, has_width, left);
        return;
    }

    ip = (uint64_t)value;
    frac_part = value - (double)ip;

    for (i = 0u; i < prec; i++) {
        scale *= 10u;
    }

    frac = (uint64_t)(frac_part * (double)scale + 0.5);
    if (frac >= scale) {
        frac = 0u;
        ip += 1u;
    }

    for (i = prec; i > 0u; i--) {
        frac_digits[i - 1u] = (char)('0' + (unsigned)(frac % 10u));
        frac /= 10u;
    }

    nd = rtt_u64_digits(ip, ip_digits);

    if (negative) {
        body[bl++] = '-';
    } else if (plus) {
        body[bl++] = '+';
    } else if (space_flag) {
        body[bl++] = ' ';
    }
    prefix_len = bl;

    rtt_append(body, &bl, ip_digits, nd);
    if (prec != 0u) {
        body[bl++] = '.';
        rtt_append(body, &bl, frac_digits, prec);
    }

    rtt_w_padded(w, body, bl, width, has_width, left, zero, prefix_len);
}

#endif /* RTT_PRINTF_SUPPORT_FLOAT */

/* 核心格式化，返回写入 out 的字符数（可能因缓冲区不足被截断）。
 * 对外公开为 rtt_vformat()：让需要在本地拼整行的调用方（rtt_log）复用本格式化器，
 * 从而不必为一个日志接口链接 newlib 的 printf（在 60 KB Flash 的器件上差 5 KB 量级）。 */
unsigned rtt_vformat(char* out, unsigned cap, const char* fmt, va_list ap)
{
    rtt_writer_t w;

    w.buf = out;
    w.cap = cap;
    w.len = 0u;

    while (*fmt != '\0') {
        int left = 0;
        int plus = 0;
        int space_flag = 0;
        int zero = 0;
        int hash = 0;
        int has_width = 0;
        int has_prec = 0;
        unsigned width = 0u;
        unsigned prec = 0u;
        char conv;

        if (*fmt != '%') {
            rtt_w_char(&w, *fmt);
            fmt++;
            continue;
        }

        fmt++; /* 跳过 '%' */

        /* 标志 */
        for (;;) {
            if (*fmt == '-') {
                left = 1;
                fmt++;
            } else if (*fmt == '+') {
                plus = 1;
                fmt++;
            } else if (*fmt == ' ') {
                space_flag = 1;
                fmt++;
            } else if (*fmt == '0') {
                zero = 1;
                fmt++;
            } else if (*fmt == '#') {
                hash = 1;
                fmt++;
            } else {
                break;
            }
        }

        /* 宽度（支持 * 动态取值） */
        if (*fmt == '*') {
            int v = va_arg(ap, int);
            fmt++;
            has_width = 1;
            if (v < 0) {
                left = 1;
                width = (unsigned)(-v);
            } else {
                width = (unsigned)v;
            }
        } else {
            while ((*fmt >= '0') && (*fmt <= '9')) {
                width = width * 10u + (unsigned)(*fmt - '0');
                fmt++;
                has_width = 1;
            }
        }
        if (width > 4096u) {
            width = 4096u; /* 防止异常格式串造成长时间循环 */
        }

        /* 精度（支持 .* 动态取值） */
        if (*fmt == '.') {
            fmt++;
            has_prec = 1;
            if (*fmt == '*') {
                int v = va_arg(ap, int);
                fmt++;
                if (v < 0) {
                    has_prec = 0;
                } else {
                    prec = (unsigned)v;
                }
            } else {
                while ((*fmt >= '0') && (*fmt <= '9')) {
                    prec = prec * 10u + (unsigned)(*fmt - '0');
                    fmt++;
                }
            }
            if (prec > 32u) {
                prec = 32u; /* 数字临时缓冲按 32 位补零设计，避免越界 */
            }
        }

        /* 长度修饰符：整型统一按 32 位处理，这里只跳过 */
        if ((*fmt == 'l') || (*fmt == 'L')) {
            fmt++;
            if (*fmt == 'l') {
                fmt++;
            }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') {
                fmt++;
            }
        } else if ((*fmt == 'j') || (*fmt == 'z') || (*fmt == 't')) {
            fmt++;
        }

        if (*fmt == '\0') {
            rtt_w_char(&w, '%'); /* 格式串以 '%' 结尾 */
            break;
        }

        conv = *fmt;

        switch (conv) {
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                unsigned uv = (v < 0) ? ((unsigned)(-(v + 1)) + 1u) : (unsigned)v;
                rtt_w_int(&w, uv, (v < 0), 10u, 0,
                          width, has_width, prec, has_prec,
                          left, zero, plus, space_flag, 0);
                break;
            }

            case 'u':
                rtt_w_int(&w, (unsigned)va_arg(ap, unsigned), 0, 10u, 0,
                          width, has_width, prec, has_prec,
                          left, zero, plus, space_flag, 0);
                break;

            case 'o':
                rtt_w_int(&w, (unsigned)va_arg(ap, unsigned), 0, 8u, 0,
                          width, has_width, prec, has_prec,
                          left, zero, plus, space_flag, hash);
                break;

            case 'x':
                rtt_w_int(&w, (unsigned)va_arg(ap, unsigned), 0, 16u, 0,
                          width, has_width, prec, has_prec,
                          left, zero, plus, space_flag, hash);
                break;

            case 'X':
                rtt_w_int(&w, (unsigned)va_arg(ap, unsigned), 0, 16u, 1,
                          width, has_width, prec, has_prec,
                          left, zero, plus, space_flag, hash);
                break;

            case 'c': {
                char c = (char)va_arg(ap, int);
                rtt_w_text(&w, &c, 1u, width, has_width, left);
                break;
            }

            case 's': {
                const char* s = va_arg(ap, const char*);
                unsigned n;
                if (s == 0) {
                    s = "(null)";
                }
                n = rtt_str_len(s);
                if (has_prec && (prec < n)) {
                    n = prec;
                }
                rtt_w_text(&w, s, n, width, has_width, left);
                break;
            }

            case 'p': {
                void* p = va_arg(ap, void*);
                unsigned v = (unsigned)(uintptr_t)p;
                if (v == 0u) {
                    /* 空指针也保持 0x + 8 位格式，直接输出固定文本 */
                    rtt_w_text(&w, "0x00000000", 10u, width, has_width, left);
                } else {
                    rtt_w_int(&w, v, 0, 16u, 0,
                              width, has_width, 8u, 1,
                              left, 0, 0, 0, 1);
                }
                break;
            }

            case 'n': {
                int* p = (int*)va_arg(ap, int*);
                if (p != 0) {
                    *p = (int)w.len;
                }
                break;
            }

            case '%':
                rtt_w_char(&w, '%');
                break;

#if RTT_PRINTF_SUPPORT_FLOAT
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                double v = va_arg(ap, double);
                /* e/E/g/G/a/A 统一按定点输出，避免参数错位与未消费参数 */
                rtt_w_float(&w, v, width, has_width, prec, has_prec,
                            left, zero, plus, space_flag);
                break;
            }
#else
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                (void)va_arg(ap, double); /* 仍然消费参数，保证后续参数不错位 */
                rtt_w_text(&w, "?", 1u, width, has_width, left);
                break;
            }
#endif

            default:
                /* 未知格式符：原样回显，不消费参数 */
                rtt_w_char(&w, '%');
                rtt_w_char(&w, conv);
                break;
        }

        fmt++;
    }

    if (cap != 0u) {
        out[w.len] = '\0';
    }

    return w.len;
}

/* ======================== 环形缓冲区辅助 ======================== */

/* 调试器可能写入非法偏移：越界时复位为 0，避免后续越界读写 */
static void rtt_fix_offsets(volatile uint32_t* wr, volatile uint32_t* rd,
                            uint32_t size)
{
    if (size == 0u) {
        return;
    }
    if ((*wr >= size) || (*rd >= size)) {
        *wr = 0u;
        *rd = 0u;
    }
}

static uint32_t rtt_used_bytes(uint32_t wr, uint32_t rd, uint32_t size)
{
    uint32_t used = (wr >= rd) ? (wr - rd) : (size + wr - rd);
    if (used >= size) {
        used = size - 1u; /* 非法占用量按“已满”处理，防止后续下溢 */
    }
    return used;
}

/* ======================== 内部实现 ======================== */

static void rtt_ensure_init(void);

/* 写入指定通道，调用前必须已持锁；返回实际写入字节数 */
static unsigned rtt_write_locked(unsigned channel, const char* data, unsigned len)
{
    rtt_up_buffer_t* p;
    uint32_t size;
    uint32_t wr;
    uint32_t rd;
    uint32_t used;
    uint32_t space;
    uint32_t first;

    if (channel >= (unsigned)RTT_NUM_UP_BUFFERS) {
        return 0u;
    }

    p = &rtt_control_block.aUp[channel];
    size = p->SizeOfBuffer;
    if (size == 0u) {
        s_dropped[channel] += len;
        return 0u;
    }

    rtt_fix_offsets(&p->WrOff, &p->RdOff, size);
    wr = p->WrOff;
    rd = p->RdOff;

    used = rtt_used_bytes(wr, rd, size);
    space = size - used - 1u; /* 空出 1 字节，用于区分空与满 */

#if RTT_WRITE_TRIM_ON_FULL
    if (len > space) {
        s_dropped[channel] += (len - space);
        len = space;
    }
    if (len == 0u) {
        return 0u;
    }
#else
    if (len > space) {
        /* Flags=0（NO_BLOCK_SKIP）：整条丢弃，避免写出半行 */
        s_dropped[channel] += len;
        return 0u;
    }
#endif

    first = size - wr;
    if (first > len) {
        first = len;
    }

    rtt_mem_copy(&p->pBuffer[wr], data, first);
    if (len > first) {
        rtt_mem_copy(p->pBuffer, data + first, len - first);
    }

    wr += len;
    if (wr >= size) {
        wr -= size;
    }

    /* 先写数据，再更新写指针，主机据此判断有效数据 */
    rtt_memory_barrier();
    p->WrOff = wr;

    return len;
}

/* ======================== 对外接口 ======================== */

void rtt_init(void)
{
    rtt_state_t state;
    unsigned i;

    if (s_initialized != 0u) {
        return;
    }

    /* 关中断后再检查一次，避免任务与中断同时首次调用时互相覆盖 */
    state = rtt_lock();

    if (s_initialized == 0u) {
        rtt_mem_set(&rtt_control_block, 0u, (unsigned)sizeof(rtt_control_block));

        rtt_control_block.MaxNumUpBuffers = (int)RTT_NUM_UP_BUFFERS;
        rtt_control_block.MaxNumDownBuffers = (int)RTT_NUM_DOWN_BUFFERS;

        for (i = 0u; i < RTT_NUM_UP_BUFFERS; i++) {
            rtt_control_block.aUp[i].sName = s_channel_name;
            rtt_control_block.aUp[i].pBuffer = s_up_buffers[i];
            rtt_control_block.aUp[i].SizeOfBuffer = RTT_BUFFER_SIZE_UP;
            rtt_control_block.aUp[i].WrOff = 0u;
            rtt_control_block.aUp[i].RdOff = 0u;
            rtt_control_block.aUp[i].Flags = 0u;
            s_dropped[i] = 0u;
        }

        for (i = 0u; i < RTT_NUM_DOWN_BUFFERS; i++) {
            rtt_control_block.aDown[i].sName = s_channel_name;
            rtt_control_block.aDown[i].pBuffer = s_down_buffers[i];
            rtt_control_block.aDown[i].SizeOfBuffer = RTT_BUFFER_SIZE_DOWN;
            rtt_control_block.aDown[i].WrOff = 0u;
            rtt_control_block.aDown[i].RdOff = 0u;
            rtt_control_block.aDown[i].Flags = 0u;
        }

        /* 标识最后写入，避免主机在控制块尚未初始化完成时误识别 */
        rtt_memory_barrier();
        rtt_mem_copy(rtt_control_block.acID, "SEGGER RTT", 10u);
        rtt_memory_barrier();

        s_initialized = 1u;
    }

    rtt_unlock(state);
}

static void rtt_ensure_init(void)
{
    if (s_initialized == 0u) {
        rtt_init();
    }
}

unsigned rtt_write_channel(unsigned channel, const void* data, unsigned len)
{
    rtt_state_t state;
    unsigned written;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_UP_BUFFERS) || (data == 0) || (len == 0u)) {
        return 0u;
    }

    state = rtt_lock();
    written = rtt_write_locked(channel, (const char*)data, len);
    rtt_unlock(state);

    return written;
}

unsigned rtt_write_string_channel(unsigned channel, const char* s)
{
    if (s == 0) {
        return 0u;
    }

    return rtt_write_channel(channel, s, rtt_str_len(s));
}

unsigned rtt_write(const void* data, unsigned len)
{
    return rtt_write_channel(0u, data, len);
}

unsigned rtt_write_string(const char* s)
{
    return rtt_write_string_channel(0u, s);
}

unsigned rtt_printf_channel(unsigned channel, const char* fmt, ...)
{
    char buffer[RTT_PRINTF_BUFFER_SIZE];
    va_list ap;
    unsigned n;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_UP_BUFFERS) || (fmt == 0)) {
        return 0u;
    }

    va_start(ap, fmt);
    n = rtt_vformat(buffer, RTT_PRINTF_BUFFER_SIZE, fmt, ap);
    va_end(ap);

    /* 格式化在锁外完成，仅写入时关中断 */
    return rtt_write_channel(channel, buffer, n);
}

unsigned rtt_printf(const char* fmt, ...)
{
    char buffer[RTT_PRINTF_BUFFER_SIZE];
    va_list ap;
    unsigned n;

    rtt_ensure_init();

    if (fmt == 0) {
        return 0u;
    }

    va_start(ap, fmt);
    n = rtt_vformat(buffer, RTT_PRINTF_BUFFER_SIZE, fmt, ap);
    va_end(ap);

    return rtt_write_channel(0u, buffer, n);
}

/* ======================== ANSI 着色 ======================== */

#if RTT_SUPPORT_ANSI

/* 追加一个 SGR 参数（自动处理分号） */
static unsigned rtt_sgr_put(char* out, unsigned n, unsigned code, int* sep)
{
    char tmp[6];
    unsigned k = 0u;

    if (*sep != 0) {
        out[n++] = ';';
    }
    *sep = 1;

    if (code == 0u) {
        out[n++] = '0';
        return n;
    }

    while (code != 0u) {
        tmp[k++] = (char)('0' + (code % 10u));
        code /= 10u;
    }
    while (k != 0u) {
        out[n++] = tmp[--k];
    }

    return n;
}

/* 16 色索引转 SGR 代码：1~8 基本色，9~16 高亮色 */
static unsigned rtt_color_code(rtt_color_t color, int background)
{
    unsigned index = (unsigned)color;
    unsigned base = (index <= 8u) ? (background ? 40u : 30u)
                                  : (background ? 100u : 90u);

    return base + ((index - 1u) & 7u);
}

/* 生成 16 色前缀，如 "\x1b[1;31;40m" */
static unsigned rtt_sgr16_build(char* out, rtt_color_t fg, rtt_color_t bg,
                                unsigned style)
{
    unsigned n = 2u;
    int sep = 0;

    out[0] = 0x1B;
    out[1] = '[';

    if ((style & RTT_STYLE_BOLD) != 0u) {
        n = rtt_sgr_put(out, n, 1u, &sep);
    }
    if (((unsigned)fg >= 1u) && ((unsigned)fg <= 16u)) {
        n = rtt_sgr_put(out, n, rtt_color_code(fg, 0), &sep);
    }
    if (((unsigned)bg >= 1u) && ((unsigned)bg <= 16u)) {
        n = rtt_sgr_put(out, n, rtt_color_code(bg, 1), &sep);
    }
    if (sep == 0) {
        n = rtt_sgr_put(out, n, 0u, &sep); /* 没有样式时输出复位 */
    }

    out[n++] = 'm';
    return n;
}

/* 生成 256 色前缀，如 "\x1b[38;5;196m" */
static unsigned rtt_sgr256_build(char* out, int fg_index, int bg_index,
                                 unsigned style)
{
    unsigned n = 2u;
    int sep = 0;

    out[0] = 0x1B;
    out[1] = '[';

    if ((style & RTT_STYLE_BOLD) != 0u) {
        n = rtt_sgr_put(out, n, 1u, &sep);
    }
    if ((fg_index >= 0) && (fg_index <= 255)) {
        n = rtt_sgr_put(out, n, 38u, &sep);
        n = rtt_sgr_put(out, n, 5u, &sep);
        n = rtt_sgr_put(out, n, (unsigned)fg_index, &sep);
    }
    if ((bg_index >= 0) && (bg_index <= 255)) {
        n = rtt_sgr_put(out, n, 48u, &sep);
        n = rtt_sgr_put(out, n, 5u, &sep);
        n = rtt_sgr_put(out, n, (unsigned)bg_index, &sep);
    }
    if (sep == 0) {
        n = rtt_sgr_put(out, n, 0u, &sep);
    }

    out[n++] = 'm';
    return n;
}

/* 生成真彩色前缀，如 "\x1b[38;2;255;128;0m" */
static unsigned rtt_sgr_rgb_build(char* out, int fg_rgb, int bg_rgb,
                                  unsigned style)
{
    unsigned n = 2u;
    int sep = 0;
    unsigned rgb;

    out[0] = 0x1B;
    out[1] = '[';

    if ((style & RTT_STYLE_BOLD) != 0u) {
        n = rtt_sgr_put(out, n, 1u, &sep);
    }
    if (fg_rgb >= 0) {
        rgb = (unsigned)fg_rgb;
        n = rtt_sgr_put(out, n, 38u, &sep);
        n = rtt_sgr_put(out, n, 2u, &sep);
        n = rtt_sgr_put(out, n, (rgb >> 16) & 0xFFu, &sep);
        n = rtt_sgr_put(out, n, (rgb >> 8) & 0xFFu, &sep);
        n = rtt_sgr_put(out, n, rgb & 0xFFu, &sep);
    }
    if (bg_rgb >= 0) {
        rgb = (unsigned)bg_rgb;
        n = rtt_sgr_put(out, n, 48u, &sep);
        n = rtt_sgr_put(out, n, 2u, &sep);
        n = rtt_sgr_put(out, n, (rgb >> 16) & 0xFFu, &sep);
        n = rtt_sgr_put(out, n, (rgb >> 8) & 0xFFu, &sep);
        n = rtt_sgr_put(out, n, rgb & 0xFFu, &sep);
    }
    if (sep == 0) {
        n = rtt_sgr_put(out, n, 0u, &sep);
    }

    out[n++] = 'm';
    return n;
}

/* 颜色前缀 + 正文 + 复位，合并为一次写入，避免与其它任务交错 */
static unsigned rtt_write_colored(unsigned channel, const char* fmt, va_list ap,
                                  const char* prefix, unsigned prefix_len)
{
    char buffer[RTT_PRINTF_BUFFER_SIZE + 64u];
    unsigned body;

    rtt_mem_copy(buffer, prefix, prefix_len);
    body = rtt_vformat(buffer + prefix_len, RTT_PRINTF_BUFFER_SIZE, fmt, ap);

    prefix_len += body;
    buffer[prefix_len++] = 0x1B;
    buffer[prefix_len++] = '[';
    buffer[prefix_len++] = '0';
    buffer[prefix_len++] = 'm';

    return rtt_write_channel(channel, buffer, prefix_len);
}

unsigned rtt_printf_color(unsigned channel, rtt_color_t fg, rtt_color_t bg,
                          unsigned style, const char* fmt, ...)
{
    char prefix[48];
    unsigned prefix_len;
    va_list ap;
    unsigned n;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_UP_BUFFERS) || (fmt == 0)) {
        return 0u;
    }

    prefix_len = rtt_sgr16_build(prefix, fg, bg, style);

    va_start(ap, fmt);
    n = rtt_write_colored(channel, fmt, ap, prefix, prefix_len);
    va_end(ap);

    return n;
}

unsigned rtt_printf_color256(unsigned channel, int fg_index, int bg_index,
                             unsigned style, const char* fmt, ...)
{
    char prefix[48];
    unsigned prefix_len;
    va_list ap;
    unsigned n;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_UP_BUFFERS) || (fmt == 0)) {
        return 0u;
    }

    prefix_len = rtt_sgr256_build(prefix, fg_index, bg_index, style);

    va_start(ap, fmt);
    n = rtt_write_colored(channel, fmt, ap, prefix, prefix_len);
    va_end(ap);

    return n;
}

unsigned rtt_printf_color_rgb(unsigned channel, int fg_rgb, int bg_rgb,
                              unsigned style, const char* fmt, ...)
{
    char prefix[48];
    unsigned prefix_len;
    va_list ap;
    unsigned n;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_UP_BUFFERS) || (fmt == 0)) {
        return 0u;
    }

    prefix_len = rtt_sgr_rgb_build(prefix, fg_rgb, bg_rgb, style);

    va_start(ap, fmt);
    n = rtt_write_colored(channel, fmt, ap, prefix, prefix_len);
    va_end(ap);

    return n;
}

unsigned rtt_ansi_color(unsigned channel, rtt_color_t fg, rtt_color_t bg,
                        unsigned style)
{
    char prefix[48];
    unsigned prefix_len;

    rtt_ensure_init();

    if (channel >= (unsigned)RTT_NUM_UP_BUFFERS) {
        return 0u;
    }

    prefix_len = rtt_sgr16_build(prefix, fg, bg, style);
    return rtt_write_channel(channel, prefix, prefix_len);
}

unsigned rtt_ansi_reset(unsigned channel)
{
    rtt_ensure_init();

    if (channel >= (unsigned)RTT_NUM_UP_BUFFERS) {
        return 0u;
    }

    return rtt_write_channel(channel, "\x1b[0m", 4u);
}

#endif /* RTT_SUPPORT_ANSI */

unsigned rtt_get_dropped_bytes(unsigned channel)
{
    rtt_state_t state;
    unsigned dropped;

    rtt_ensure_init();

    if (channel >= (unsigned)RTT_NUM_UP_BUFFERS) {
        return 0u;
    }

    state = rtt_lock();
    dropped = (unsigned)s_dropped[channel];
    rtt_unlock(state);

    return dropped;
}

void rtt_reset_dropped_bytes(unsigned channel)
{
    rtt_state_t state;

    rtt_ensure_init();

    if (channel >= (unsigned)RTT_NUM_UP_BUFFERS) {
        return;
    }

    state = rtt_lock();
    s_dropped[channel] = 0u;
    rtt_unlock(state);
}

int rtt_set_channel_name(unsigned channel, const char* name)
{
    rtt_state_t state;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_UP_BUFFERS) || (name == 0)) {
        return -1;
    }

    state = rtt_lock();
    rtt_control_block.aUp[channel].sName = name;
    rtt_unlock(state);

    return 0;
}

int rtt_get_char_channel(unsigned channel)
{
    char c = 0;
    unsigned n;

    n = rtt_read_channel(channel, &c, 1u);
    if (n != 1u) {
        return -1;
    }

    return (int)(unsigned char)c;
}

int rtt_get_char(void)
{
    return rtt_get_char_channel(0u);
}

unsigned rtt_read_channel(unsigned channel, void* buf, unsigned max_len)
{
    rtt_down_buffer_t* p;
    rtt_state_t state;
    uint32_t size;
    uint32_t wr;
    uint32_t rd;
    uint32_t used;
    uint32_t first;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_DOWN_BUFFERS) || (buf == 0) || (max_len == 0u)) {
        return 0u;
    }

    p = &rtt_control_block.aDown[channel];

    state = rtt_lock();

    size = p->SizeOfBuffer;
    if (size == 0u) {
        rtt_unlock(state);
        return 0u;
    }

    rtt_fix_offsets(&p->WrOff, &p->RdOff, size);
    wr = p->WrOff;
    rd = p->RdOff;

    /* 可读量必须限制在缓冲区容量内，防止调试器写入非法 WrOff 时越界读 */
    used = rtt_used_bytes(wr, rd, size);
    if (used > max_len) {
        used = max_len;
    }

    if (used != 0u) {
        first = size - rd;
        if (first > used) {
            first = used;
        }
        rtt_mem_copy(buf, &p->pBuffer[rd], first);
        if (used > first) {
            rtt_mem_copy((char*)buf + first, p->pBuffer, used - first);
        }

        rd += used;
        if (rd >= size) {
            rd -= size;
        }

        rtt_memory_barrier();
        p->RdOff = rd;
    }

    rtt_unlock(state);

    return used;
}

unsigned rtt_read(void* buf, unsigned max_len)
{
    return rtt_read_channel(0u, buf, max_len);
}

unsigned rtt_available_channel(unsigned channel)
{
    rtt_down_buffer_t* p;
    rtt_state_t state;
    uint32_t size;
    uint32_t wr;
    uint32_t rd;
    uint32_t used;

    rtt_ensure_init();

    if (channel >= (unsigned)RTT_NUM_DOWN_BUFFERS) {
        return 0u;
    }

    p = &rtt_control_block.aDown[channel];

    state = rtt_lock();

    size = p->SizeOfBuffer;
    if (size == 0u) {
        rtt_unlock(state);
        return 0u;
    }

    rtt_fix_offsets(&p->WrOff, &p->RdOff, size);
    wr = p->WrOff;
    rd = p->RdOff;
    used = rtt_used_bytes(wr, rd, size);

    rtt_unlock(state);

    return used;
}

int rtt_set_down_channel_name(unsigned channel, const char* name)
{
    rtt_state_t state;

    rtt_ensure_init();

    if ((channel >= (unsigned)RTT_NUM_DOWN_BUFFERS) || (name == 0)) {
        return -1;
    }

    state = rtt_lock();
    rtt_control_block.aDown[channel].sName = name;
    rtt_unlock(state);

    return 0;
}

int rtt_has_data(void)
{
    return (rtt_available_channel(0u) != 0u) ? 1 : 0;
}

void* rtt_get_control_block(void)
{
    return (void*)&rtt_control_block;
}
