/**
 * @file    rtt.h
 * @brief   RTT（Real Time Transfer）
 *
 * 特点：
 *   1. 无汇编文件、无 HAL/RTOS/CMSIS 依赖；
 *   2. 兼容 SEGGER RTT 协议，主机在 RAM 中搜索 "SEGGER RTT" 即可识别；
 *   3. 多上行 / 多下行通道，满足监视、多通道显示、下行发送等全部功能；
 *      另提供 ANSI 着色接口（16 色 / 256 色 / 真彩色 / 加粗），
 *      “文本 + ANSI”模式可直接渲染；
 *   4. 控制器：GCC/Clang（arm-none-eabi、armclang）、ARM Compiler 5、IAR EWARM；
 *      其它编译器必须显式定义 RTT_NO_CRITICAL_SECTION 才能编译（避免静默失去保护）；
 *   5. 控制块是普通全局变量，无需链接脚本或指定段。
 *
 * 移植步骤：
 *   1. 把 rtt.c、rtt.h 加入工程编译；
 *   2. 在 main() 初始化阶段调用一次 rtt_init()（不调用也会在首次使用时自动初始化）；
 *   3. 输出：rtt_printf("count=%d\n", n); 或 rtt_write_channel(1, buf, len);
 *      输入：rtt_get_char() / rtt_read_channel(0, buf, size);
 *   4. 打开本工具“监视”页面连接探针即可看到数据。
 *
 * 数据格式：
 *   每行一个样本，样本内多个通道值用冒号分隔，例如 "11:22:33\n"。
 */

#ifndef RTT_H
#define RTT_H

/* ============================ 可配置项 ============================
 * 可在工程编译宏中覆盖（例如 -DRTT_NUM_UP_BUFFERS=2u），无需改动源码。
 */

/* 上行通道数量（目标 -> 主机），默认 3 个 */
#ifndef RTT_NUM_UP_BUFFERS
#define RTT_NUM_UP_BUFFERS 3u
#endif

/* 下行通道数量（主机 -> 目标），默认 3 个 */
#ifndef RTT_NUM_DOWN_BUFFERS
#define RTT_NUM_DOWN_BUFFERS 3u
#endif

/* 单个上行缓冲区大小（字节），3686 字节 RAM 可考虑 256，性能优先可加 1024 */
#ifndef RTT_BUFFER_SIZE_UP
#define RTT_BUFFER_SIZE_UP 512u
#endif

/* 单个下行缓冲区大小（字节），只用于接收主机下发的短命令，16 ~ 64 足够 */
#ifndef RTT_BUFFER_SIZE_DOWN
#define RTT_BUFFER_SIZE_DOWN 32u
#endif

/* 是否支持 printf 浮点（%f/%e/%g）。MCU 无 FPU 时会引入软件浮点运算，代码量增加 */
#ifndef RTT_PRINTF_SUPPORT_FLOAT
#define RTT_PRINTF_SUPPORT_FLOAT 1
#endif

/* printf 单次格式化使用的临时缓冲（栈上分配），超长格式串会被截断 */
#ifndef RTT_PRINTF_BUFFER_SIZE
#define RTT_PRINTF_BUFFER_SIZE 256u
#endif

/* 提供 ANSI 着色接口（“文本 + ANSI”模式可解析；设备端只是输出字节流） */
#ifndef RTT_SUPPORT_ANSI
#define RTT_SUPPORT_ANSI 1
#endif

/* 缓冲区满时的写入策略：
 *   0 = 整条丢弃（默认，和 Flags=0 的 SEGGER 语义一致，不会出现半行）
 *   1 = 能写多少写多少（保留旧行为）
 * 两种情况都会通过 rtt_get_dropped_bytes() 统计丢失字节数 */
#ifndef RTT_WRITE_TRIM_ON_FULL
#define RTT_WRITE_TRIM_ON_FULL 0
#endif

#if (RTT_NUM_UP_BUFFERS < 1u) || (RTT_NUM_UP_BUFFERS > 16u)
#error "RTT_NUM_UP_BUFFERS must be in range 1..16"
#endif
#if (RTT_NUM_DOWN_BUFFERS < 1u) || (RTT_NUM_DOWN_BUFFERS > 16u)
#error "RTT_NUM_DOWN_BUFFERS must be in range 1..16"
#endif
#if (RTT_BUFFER_SIZE_UP < 64u)
#error "RTT_BUFFER_SIZE_UP must be >= 64"
#endif
#if (RTT_BUFFER_SIZE_DOWN < 8u)
#error "RTT_BUFFER_SIZE_DOWN must be >= 8"
#endif
#if (RTT_PRINTF_BUFFER_SIZE < 32u)
#error "RTT_PRINTF_BUFFER_SIZE must be >= 32"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 RTT 控制块与各通道缓冲区
 * @note   可重复调用；内部关中断保护，任务与中断同时首次调用不会互相破坏。
 */
void rtt_init(void);

/* ------------------------------ 上行（目标 -> 主机） ------------------------------ */

/**
 * @brief  写入原始数据到指定上行通道
 * @param  channel  通道号（0 ~ RTT_NUM_UP_BUFFERS-1）
 * @param  data     数据首地址
 * @param  len      字节数
 * @retval 实际写入字节数
 * @note   默认非阻塞、整条写入或整条丢弃：空间不足时不写入并返回 0，
 *         丢失字节数可用 rtt_get_dropped_bytes() 查询。
 */
unsigned rtt_write_channel(unsigned channel, const void* data, unsigned len);

/**
 * @brief  写入以 '\0' 结尾的字符串到指定上行通道
 * @retval 实际写入字节数
 */
unsigned rtt_write_string_channel(unsigned channel, const char* s);

/**
 * @brief  写入原始数据到通道 0
 */
unsigned rtt_write(const void* data, unsigned len);

/**
 * @brief  写入字符串到通道 0
 */
unsigned rtt_write_string(const char* s);

/**
 * @brief  格式化输出到通道 0
 * @param  fmt  格式串，支持：
 *              %d %i %u %o %x %X %c %s %p %% %n %f %F %e %E %g %G %a %A
 *              标志 - + 空格 0 #，宽度与精度（含 * 动态取值），
 *              长度修饰 l/ll/h/hh/L/j/z/t（整型统一按 32 位处理）
 * @note   浮点需 RTT_PRINTF_SUPPORT_FLOAT=1；e/E/g/G 按定点格式输出。
 *         格式化在锁外完成，仅在写入时关中断，避免长时间屏蔽中断。
 * @retval 实际写入字节数
 */
unsigned rtt_printf(const char* fmt, ...);

/**
 * @brief  格式化输出到指定上行通道
 */
unsigned rtt_printf_channel(unsigned channel, const char* fmt, ...);

/* ------------------------------ ANSI 着色 ------------------------------ */
#if RTT_SUPPORT_ANSI

/**
 * @brief  ANSI 16 色
 * @note   0 表示不改变当前颜色；9 ~ 16 为高亮色。
 *         固件输出的是标准 ANSI 转义字节，工具“文本 + ANSI”模式会渲染颜色，
 *         “文本”模式则原样显示转义序列。
 */
typedef enum {
    RTT_COLOR_DEFAULT = 0,
    RTT_COLOR_BLACK = 1,
    RTT_COLOR_RED = 2,
    RTT_COLOR_GREEN = 3,
    RTT_COLOR_YELLOW = 4,
    RTT_COLOR_BLUE = 5,
    RTT_COLOR_MAGENTA = 6,
    RTT_COLOR_CYAN = 7,
    RTT_COLOR_WHITE = 8,
    RTT_COLOR_BRIGHT_BLACK = 9,
    RTT_COLOR_BRIGHT_RED = 10,
    RTT_COLOR_BRIGHT_GREEN = 11,
    RTT_COLOR_BRIGHT_YELLOW = 12,
    RTT_COLOR_BRIGHT_BLUE = 13,
    RTT_COLOR_BRIGHT_MAGENTA = 14,
    RTT_COLOR_BRIGHT_CYAN = 15,
    RTT_COLOR_BRIGHT_WHITE = 16
} rtt_color_t;

/* 文本样式位，可传给所有带颜色的 printf 接口 */
#define RTT_STYLE_NONE 0x00u
#define RTT_STYLE_BOLD 0x01u

/* 可直接拼进普通 rtt_printf 的 ANSI 转义宏 */
#define RTT_ANSI_RESET        "\x1b[0m"
#define RTT_ANSI_BOLD         "\x1b[1m"
#define RTT_ANSI_FG_BLACK     "\x1b[30m"
#define RTT_ANSI_FG_RED       "\x1b[31m"
#define RTT_ANSI_FG_GREEN     "\x1b[32m"
#define RTT_ANSI_FG_YELLOW    "\x1b[33m"
#define RTT_ANSI_FG_BLUE      "\x1b[34m"
#define RTT_ANSI_FG_MAGENTA   "\x1b[35m"
#define RTT_ANSI_FG_CYAN      "\x1b[36m"
#define RTT_ANSI_FG_WHITE     "\x1b[37m"
#define RTT_ANSI_FG_GRAY      "\x1b[90m"
#define RTT_ANSI_FG_BRED      "\x1b[91m"
#define RTT_ANSI_FG_BGREEN    "\x1b[92m"
#define RTT_ANSI_FG_BYELLOW   "\x1b[93m"
#define RTT_ANSI_FG_BBLUE     "\x1b[94m"
#define RTT_ANSI_FG_BMAGENTA  "\x1b[95m"
#define RTT_ANSI_FG_BCYAN     "\x1b[96m"
#define RTT_ANSI_FG_BWHITE    "\x1b[97m"
#define RTT_ANSI_BG_BLACK     "\x1b[40m"
#define RTT_ANSI_BG_RED       "\x1b[41m"
#define RTT_ANSI_BG_GREEN     "\x1b[42m"
#define RTT_ANSI_BG_YELLOW    "\x1b[43m"
#define RTT_ANSI_BG_BLUE      "\x1b[44m"
#define RTT_ANSI_BG_MAGENTA   "\x1b[45m"
#define RTT_ANSI_BG_CYAN      "\x1b[46m"
#define RTT_ANSI_BG_WHITE     "\x1b[47m"
#define RTT_ANSI_BG_GRAY      "\x1b[100m"
#define RTT_ANSI_BG_BRED      "\x1b[101m"
#define RTT_ANSI_BG_BGREEN    "\x1b[102m"
#define RTT_ANSI_BG_BYELLOW   "\x1b[103m"
#define RTT_ANSI_BG_BBLUE     "\x1b[104m"
#define RTT_ANSI_BG_BMAGENTA  "\x1b[105m"
#define RTT_ANSI_BG_BCYAN     "\x1b[106m"
#define RTT_ANSI_BG_BWHITE    "\x1b[107m"

/**
 * @brief  带 16 色/加粗的一次性格式化输出
 * @param  fg    前景色，RTT_COLOR_DEFAULT 表示不设置
 * @param  bg    背景色，RTT_COLOR_DEFAULT 表示不设置
 * @param  style RTT_STYLE_NONE 或 RTT_STYLE_BOLD
 * @note   颜色前缀、正文、复位序列作为一次写入提交，不会与其它任务交错。
 * @retval 实际写入字节数（含 ANSI 序列）
 */
unsigned rtt_printf_color(unsigned channel, rtt_color_t fg, rtt_color_t bg,
                          unsigned style, const char* fmt, ...);

/**
 * @brief  256 色一次性格式化输出
 * @param  fg_index  前景 0~255，-1 表示不设置
 * @param  bg_index  背景 0~255，-1 表示不设置
 */
unsigned rtt_printf_color256(unsigned channel, int fg_index, int bg_index,
                             unsigned style, const char* fmt, ...);

/**
 * @brief  真彩色一次性格式化输出
 * @param  fg_rgb  前景 0xRRGGBB，-1 表示不设置
 * @param  bg_rgb  背景 0xRRGGBB，-1 表示不设置
 */
unsigned rtt_printf_color_rgb(unsigned channel, int fg_rgb, int bg_rgb,
                              unsigned style, const char* fmt, ...);

/**
 * @brief  只输出设置颜色的转义序列（不输出文本）
 * @retval 实际写入字节数
 */
unsigned rtt_ansi_color(unsigned channel, rtt_color_t fg, rtt_color_t bg,
                        unsigned style);

/**
 * @brief  只输出颜色复位序列
 */
unsigned rtt_ansi_reset(unsigned channel);

#endif /* RTT_SUPPORT_ANSI */

/**
 * @brief  查询指定上行通道因缓冲区满而丢弃的字节数
 */
unsigned rtt_get_dropped_bytes(unsigned channel);

/**
 * @brief  清零指定上行通道的丢弃计数
 */
void rtt_reset_dropped_bytes(unsigned channel);

/**
 * @brief  设置指定上行通道的名称（name 必须是静态存储期字符串）
 * @retval 0 成功，-1 通道非法或 name 为空
 */
int rtt_set_channel_name(unsigned channel, const char* name);

/* ------------------------------ 下行（主机 -> 目标） ------------------------------ */

/**
 * @brief  从指定下行通道读取一个字节
 * @retval 读到的字节（0~255）；无数据或通道非法时返回 -1
 */
int rtt_get_char_channel(unsigned channel);

/**
 * @brief  从通道 0 下行缓冲区读取一个字节
 * @retval 读到的字节（0~255）；无数据时返回 -1
 */
int rtt_get_char(void);

/**
 * @brief  从指定下行通道批量读取数据
 * @param  channel  通道号（0 ~ RTT_NUM_DOWN_BUFFERS-1）
 * @param  buf      接收缓冲区
 * @param  max_len  最多读取字节数
 * @retval 实际读取字节数（0 表示当前无数据）；不会超过缓冲区可用空间
 */
unsigned rtt_read_channel(unsigned channel, void* buf, unsigned max_len);

/**
 * @brief  从通道 0 批量读取数据
 */
unsigned rtt_read(void* buf, unsigned max_len);

/**
 * @brief  查询指定下行通道当前可读字节数
 */
unsigned rtt_available_channel(unsigned channel);

/**
 * @brief  设置指定下行通道的名称（name 必须是静态存储期字符串）
 * @retval 0 成功，-1 通道非法或 name 为空
 */
int rtt_set_down_channel_name(unsigned channel, const char* name);

/**
 * @brief  查询通道 0 是否有下行数据
 * @retval 1 有数据，0 无数据
 */
int rtt_has_data(void);

/**
 * @brief  获取控制块地址（供调试器脚本或自定义主机定位，无需暴露内部结构）
 */
void* rtt_get_control_block(void);

#ifdef __cplusplus
}
#endif

#endif /* RTT_H */
