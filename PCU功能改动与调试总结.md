# PCU 功能改动与调试总结

## 1. 项目目标

本项目使用 STM32H743 控制板连接 PCU 伺服驱动器和编码器，实现：

- UART4/J10 与 PCU 驱动器通信；
- 读取驱动器状态和编码器角度；
- 通过 USART3/J7 终端发送控制命令；
- 执行电压、电流、自动找零和故障清除；
- 基于编码器反馈实现相对角度位置控制。

| 功能 | 接口 | MCU 串口 | 引脚 | 波特率 |
|---|---|---|---|---:|
| GSA200 陀螺仪 | J5 | USART2 | PA2 / PA3 | 460800 |
| PCU 终端 | J7 | USART3 | PB10 / PB11 | 921600 |
| 烧录及 Bootloader | J9 | USART1 | PA9 / PA10 | 115200 |
| PCU 伺服驱动器 | J10 | UART4 | PC10 / PC11 | 921600 |

## 2. 新增 PCU 通信模块

新增文件：

- `App/pcu.c`
- `App/pcu.h`

主要功能：

- 初始化 UART4 和 PCU 设备 ID，默认 ID 为 `0x55`；
- 接收并解析 PCU 的 7 字节状态帧；
- 检查帧头、设备 ID 和校验和；
- 解析驱动器状态、故障位和 24 位编码器角度；
- 将编码器原始值换算为 `0~360°`；
- 发送 PCU 控制帧。

协议帧格式：

```text
上传：AA ID STATUS ANGLE_H ANGLE_M ANGLE_L CHECKSUM
下发：AA ID ENABLE MODE DATA_L DATA_H CHECKSUM
```

支持模式：`1` 电压开环、`2` 电流闭环、`3` 自动找零、`6` 故障清除。

## 3. 新增终端控制模块

新增文件：

- `App/pcu_terminal.c`
- `App/pcu_terminal.h`

USART3/J7 终端支持：

```text
help
status
set voltage <raw>
set current <raw>
enable
disable
stop
clear
zero <raw>
move <deg>
position status
position stop
```

其中：

- `set voltage`、`set current` 只暂存设定值；
- `enable` 才发送使能控制帧；
- `disable`、`stop` 关闭输出；
- `clear` 失能后清除故障；
- `zero` 执行 PCU 电机与编码器电气零点校准；
- `move` 执行相对当前位置的角度移动；
- `status` 显示角度、状态、故障和位置环信息。

终端还增加了回车执行、Backspace/Delete 删除本地输入、非法字符过滤、长度检查和参数范围检查。

## 4. `printf` 和主循环修改

在 `App/main.c` 中：

- 增加 `fputc()` 和 `_write()`，将 `printf()` 重定向到 USART3；
- 初始化 `pcu_terminal`、`pcu` 和 `pcu_position`；
- 主循环增加 `pcu_poll()`、`pcu_position_poll()` 和 `pcu_terminal_poll()`。

新增调用关系：

```c
pcu_poll();
pcu_position_poll(u32_timer_1ms);
pcu_terminal_poll();
```

## 5. 新增相对角度位置环

新增文件：

- `App/pcu_position.c`
- `App/pcu_position.h`

控制流程：

```text
编码器反馈 -> 计算角度误差 -> 比例控制 -> 电流闭环指令 -> PCU
```

当前参数：

```text
控制周期       5 ms
位置容差       0.5 deg
比例系数       3000 raw/deg
最大电流       3000 raw
方向系数       +1
反馈超时       5 s
运动超时       10 s
稳定时间       100 ms
```

位置环状态包括 `IDLE`、`MOVING`、`REACHED`、`FAULT` 和 `TIMEOUT`。每次执行 `move` 时，会使用最新的 PCU 角度重新建立当前位置基准，避免手动电流控制后使用旧角度。

## 6. UART4 DMA 发送修复

修改文件：`Bsp/UART/bsp_uart.c`。

UART4 继续使用 DMA1 Stream6 发送、Stream7 接收。 `uart4_transmit()` 增加了发送完成等待，避免上一帧尚未发完时覆盖共享 DMA 缓冲区。

该修复主要保证以下连续操作可靠：

```text
disable -> zero
```

## 7. PlatformIO 和文档修改

`platformio.ini` 增加了 `pcu.c`、`pcu_position.c` 和 `pcu_terminal.c`，并将终端监视器配置为 `921600 8N1`，同时指定自定义启动文件、系统文件和 hard-float 构建脚本。

新增或更新文档：

- `App/UART使用资源.md`：整理串口、引脚、DMA 和业务占用情况；
- `项目联合调试交接说明.md`：整理 PCU 协议、串口分配、测试顺序和调试边界；
- 本文档：总结本轮功能改动和调试结果。

## 8. 自动找零说明

PCU 的 `MODE=3` 自动找零不是把机械当前位置设置为用户的 `0°`，而是完成电机与编码器的电气零点校准和对相。

推荐流程：

```text
disable
clear
status
zero 6553
等待至少 5 秒
status
```

`6553` 约为母线电压的 20%，`8192` 约为 25%。找零电流建议约为电机额定电流的 `0.3~0.7` 倍。找零过程中电机会出力并锁定，完成后应自动失能；如果改变电机或编码器机械安装位置，需要重新找零。

## 9. 已完成验证

- PlatformIO 编译成功；
- 固件成功烧录并校验；
- UART4 可以稳定接收 PCU 状态帧；
- 编码器角度可以正常读取；
- `valid=1`，校验和错误和格式错误为 0；
- `zero 6553` 自动找零成功；
- 找零结束后驱动器自动失能；
- 找零后 `status=0x00`，无角度、过流、过温、过载、欠压和过压故障。

一次实际找零后的典型反馈为：`valid=1`、`angle≈272.389 deg`、`status=0x00`、`enabled=0`、所有故障位为 0。

## 10. 当前限制和后续工作

PCU 通信、编码器读取和自动找零已经验证成功，但 `move <deg>` 相对角度位置控制还没有最终验证到稳定成功。

目前需要继续处理：

- 根据电机负载和静摩擦调整比例系数与最大电流；
- 验证正负方向和多个角度范围；
- 增加机械限位或软件角度限位；
- 区分电气找零和机械基准回零；
- 位置环稳定后再融合 GSA200 速度信息。

当前实现是“相对角度移动”，不是带限位开关的绝对机械回零。

