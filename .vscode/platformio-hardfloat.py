Import("env")

# Keep PlatformIO's compile and link ABI consistent with the STM32H743 FPU.
env.Append(
    LINKFLAGS=[
        "-mfpu=fpv5-d16",
        "-mfloat-abi=hard",
    ]
)
