#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate compile_commands.json at the workspace root for clangd
(ST stm32cube-ide-clangd), targeting the STM32H743 GCC (arm-none-eabi) build.

The source/include/define lists mirror .vscode/build-gcc.ps1, so after
adding/removing a source file there, re-run:
    python .vscode/gen-compile-commands.py
"""
import glob
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ROOT_FWD = ROOT.replace("\\", "/")


def find_toolchain():
    candidates = [
        os.environ.get("ARM_GCC_TOOLCHAIN", ""),
        r"D:/software/arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi",
        r"C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1",
    ]
    for c in candidates:
        if c and os.path.isfile(os.path.join(c, "bin", "arm-none-eabi-gcc.exe")):
            return c.replace("\\", "/")
    return None


# Keep in sync with the $sources array in build-gcc.ps1
sources = [
    "Project/GCC/startup_stm32h743xx.c",
    "App/main.c",
    "App/boot_jump.c",
    "App/gsa200.c",
    "Bsp/LED/bsp_led.c",
    "Bsp/UART/bsp_uart.c",
    "Bsp/system_stm32h7xx.c",
    "Driver/Src/stm32h7xx_ll_dma.c",
    "Driver/Src/stm32h7xx_ll_gpio.c",
    "Driver/Src/stm32h7xx_ll_i2c.c",
    "Driver/Src/stm32h7xx_ll_pwr.c",
    "Driver/Src/stm32h7xx_ll_rcc.c",
    "Driver/Src/stm32h7xx_ll_spi.c",
    "Driver/Src/stm32h7xx_ll_usart.c",
    "Driver/Src/stm32h7xx_ll_utils.c",
]

# Keep in sync with the $includes array in build-gcc.ps1
includes = [
    "App",
    "Bsp",
    "Bsp/LED",
    "Bsp/UART",
    "CMSIS/Inc",
    "Driver/Inc",
    "Driver/Inc/Legacy",
    "STM32H7xx_Device/Inc",
]

defines = ["STM32H743xx", "USE_FULL_LL_DRIVER", "RAM_DEBUG=0"]

# Cortex-M7 flags understood by both GCC and clang.
common = [
    "-std=gnu11",
    "-mcpu=cortex-m7",
    "-mthumb",
    "-mfpu=fpv5-d16",
    "-mfloat-abi=hard",
    "-O0",
    "-g3",
]
common += ["-D" + d for d in defines]
common += ["-I" + ROOT_FWD + "/" + i for i in includes]

toolchain = find_toolchain()
if toolchain:
    # Use the real cross-GCC as driver. It already targets arm-none-eabi, so no
    # --target flag is passed (that would make clangd's driver query fail).
    # clangd's --query-driver then extracts the exact system headers + macros.
    base = [toolchain + "/bin/arm-none-eabi-gcc.exe"] + common
    # Explicit -isystem is a fallback in case query-driver is not allowed.
    system_dirs = [toolchain + "/arm-none-eabi/include"]
    for gcc_inc in sorted(glob.glob(toolchain + "/lib/gcc/arm-none-eabi/*/include")):
        system_dirs.append(gcc_inc)
        fixed = gcc_inc + "-fixed"
        if os.path.isdir(fixed):
            system_dirs.append(fixed)
    for d in system_dirs:
        base += ["-isystem", d]
    print("toolchain: " + toolchain)
else:
    # No cross-GCC: let clangd's bundled clang parse with an explicit target.
    base = ["clang", "--target=arm-none-eabi"] + common
    print("WARNING: arm-none-eabi-gcc not found; using clang with no newlib headers")

db = []
for src in sources:
    full = ROOT_FWD + "/" + src
    db.append({
        "directory": ROOT_FWD,
        "file": full,
        "arguments": base + [full],
    })

out = os.path.join(ROOT, "compile_commands.json")
with open(out, "w", encoding="utf-8", newline="\n") as fp:
    json.dump(db, fp, indent=2)
    fp.write("\n")

missing = [s for s in sources if not os.path.isfile(os.path.join(ROOT, s.replace("/", os.sep)))]
print("wrote %s (%d entries)" % (out, len(db)))
if missing:
    print("WARNING: source files not found:")
    for m in missing:
        print("  " + m)
