#ifndef PCU_H
#define PCU_H

#include <stdint.h>

/* PCU 主控与驱动器串口通信协议，版本 V1.0。 */
/* UART4 使用 921600 baud、8 数据位、无校验、1 停止位。 */
#define PCU_FRAME_SIZE             (7U)
#define PCU_FRAME_HEADER           (0xAAU)
#define PCU_DEFAULT_DEVICE_ID      (0x55U)
#define PCU_DEVICE_ID_BASE         (0x54U)
#define PCU_ANGLE_RAW_MAX          (0xFFFFFFUL)
#define PCU_SETPOINT_MIN           (-32767)
#define PCU_SETPOINT_MAX           (32767)

/* 驱动器状态字节，即状态帧的第 3 个字节。
 * 对于故障位，bit = 1 表示故障有效；对于使能位，bit = 1 表示已使能。 */
#define PCU_STATUS_ANGLE_FAULT     (1U << 0)
#define PCU_STATUS_OVERCURRENT     (1U << 1)
#define PCU_STATUS_ENABLED         (1U << 2)
#define PCU_STATUS_OVERTEMPERATURE (1U << 3)
#define PCU_STATUS_MOTOR_OVERLOAD  (1U << 4)
#define PCU_STATUS_RESERVED5       (1U << 5)
#define PCU_STATUS_BUS_UNDERVOLT   (1U << 6)
#define PCU_STATUS_BUS_OVERVOLT    (1U << 7)

/* 控制帧第 3 个字节：输出使能。 */
#define PCU_OUTPUT_DISABLED        (0U)
#define PCU_OUTPUT_ENABLED         (1U)

/* 控制帧第 4 个字节：控制模式。 */
typedef enum
{
    PCU_CONTROL_VOLTAGE_OPEN_LOOP   = 1U,
    PCU_CONTROL_CURRENT_CLOSED_LOOP = 2U,
    PCU_CONTROL_AUTO_ZERO           = 3U,
    PCU_CONTROL_FAULT_CLEAR         = 6U,
    PCU_CONTROL_RESERVED_7          = 7U,
    PCU_CONTROL_RESERVED_8          = 8U,
    PCU_CONTROL_RESERVED_9          = 9U
} pcu_control_mode_t;

/* 驱动器回传状态帧，共 7 字节：
 * AA | ID | STATUS | ANGLE[23:16] | ANGLE[15:8] | ANGLE[7:0] | CHECKSUM。 */
typedef struct
{
    uint8_t header;
    uint8_t device_id;
    uint8_t status;
    uint8_t angle_msb;
    uint8_t angle_mid;
    uint8_t angle_lsb;
    uint8_t checksum;
} pcu_status_frame_t;

/* 主控发送控制帧，共 7 字节：
 * AA | ID | ENABLE | MODE | SETPOINT_LSB | SETPOINT_MSB | CHECKSUM。 */
typedef struct
{
    uint8_t header;
    uint8_t device_id;
    uint8_t output_enable;
    uint8_t control_mode;
    uint8_t setpoint_lsb;
    uint8_t setpoint_msb;
    uint8_t checksum;
} pcu_command_frame_t;

/* 最近一次解析成功的驱动器状态和通信统计信息。 */
typedef struct
{
    uint8_t device_id;
    uint8_t status;
    uint32_t angle_raw;
    float angle_deg;
    uint32_t valid_frames;
    uint32_t checksum_errors;
    uint32_t format_errors;
    uint8_t data_valid;
    uint8_t last_frame[PCU_FRAME_SIZE];
} pcu_status_t;

extern volatile pcu_status_t g_pcu_status;

/* 初始化 UART4，并清零 PCU 解析状态和统计信息。 */
void pcu_init(uint32_t baudrate, uint8_t device_id);

/* 处理 UART4 DMA 接收完成的数据，并解析状态帧。
 * 该函数应在主循环中周期性调用。 */
void pcu_poll(void);

/* 组包并发送一帧 7 字节控制命令。
 * 返回 1 表示成功提交到 UART4 DMA，返回 0 表示参数非法或发送长度不正确。 */
uint8_t pcu_send_command(uint8_t output_enable,
                         pcu_control_mode_t control_mode,
                         int16_t setpoint);

/* 常用控制命令的快捷接口。 */
uint8_t pcu_set_voltage(int16_t setpoint);
uint8_t pcu_set_current(int16_t setpoint);
uint8_t pcu_disable_output(void);
uint8_t pcu_auto_zero(int16_t zero_voltage);
uint8_t pcu_clear_fault(void);

/* 协议辅助函数：校验和、角度编码和设定值编码。 */
uint8_t pcu_checksum(const uint8_t frame[PCU_FRAME_SIZE]);
uint8_t pcu_checksum_valid(const uint8_t frame[PCU_FRAME_SIZE]);
uint8_t pcu_frame_header_valid(const uint8_t frame[PCU_FRAME_SIZE]);
uint32_t pcu_angle_raw_get(const pcu_status_frame_t *frame);
void pcu_angle_raw_set(pcu_status_frame_t *frame, uint32_t raw);
int16_t pcu_setpoint_get(const pcu_command_frame_t *frame);
void pcu_setpoint_set(pcu_command_frame_t *frame, int16_t value);

#endif /* PCU_H */
