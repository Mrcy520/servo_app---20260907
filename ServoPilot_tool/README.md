# ServoPilot

ServoPilot 是配套 STM32 伺服控制板的 Windows 上位机，使用 USART3/J7 和自定义二进制 Protocol V1 通信。

## 已实现功能

- 串口枚举、921600 8N1连接和HELLO握手；
- `A5 5A`帧头、长度字段、CRC16-CCITT-FALSE；
- 分包、粘包、错误字节重同步；
- 请求序号、应答匹配、超时和重发；
- 全局停止、PCU清故障和自动找零；
- 手动电压开环/电流闭环暂存及使能；
- GSA200零偏校准和进度显示；
- 速度目标、反馈源、Kp、Ki、最大电流；
- 正弦速度参数、启动和停止；
- 相对位置、位置Kp、最大速度及到位模式；
- PCU、速度环、位置环、陀螺仪和通信状态显示；
- 通信心跳、板端通信看门狗设置和十六进制日志。

## 重要说明

当前STM32工程仍使用文本终端解析器。ServoPilot已经按Protocol V1实现完成，但只有在STM32端增加相同的二进制协议解析模块后才能与控制板联机。

协议定义见 [docs/protocol_v1.md](docs/protocol_v1.md)。

## 运行环境

- Windows 10/11；
- Python 3.10或更高版本；
- PySide6；
- pyserial。

## 第一次运行

在PowerShell进入本目录：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\run.ps1 -Install
```

脚本会在本目录创建独立的 `.venv`，安装依赖并启动程序。

后续启动：

```powershell
.\run.ps1
```

也可以手动运行：

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe main.py
```

## 离线协议测试

协议测试不需要连接控制板：

```powershell
py -3 -m unittest discover -s tests -v
```

## 打包为EXE

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\run.ps1 -Install
.\build_exe.ps1
```

输出目录：

```text
dist\ServoPilot\
```

## 建议联机顺序

1. 连接J7/USART3并选择对应COM口；
2. 点击“连接”，等待HELLO握手成功；
3. 确认PCU反馈有效、无故障；
4. 先使用较小的最大电流；
5. 先验证手动小电流和反馈方向；
6. 再调试速度环，最后调试位置环；
7. 紧急情况下点击顶部红色“全局停止/失能”。

## 目录结构

```text
ServoPilot_tool/
├─ main.py                    程序入口
├─ run.ps1                   环境安装和启动
├─ build_exe.ps1             EXE打包脚本
├─ requirements.txt          运行依赖
├─ docs/
│  └─ protocol_v1.md         二进制协议定义
├─ servopilot/
│  ├─ app.py                 Qt应用入口
│  ├─ protocol/              协议常量、CRC、帧编解码、命令和状态解析
│  ├─ transport/             串口线程、请求跟踪及超时重发
│  └─ ui/                    主界面和样式
└─ tests/                    协议离线测试
```

