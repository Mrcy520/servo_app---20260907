param(
    [Parameter(Mandatory = $false)]
    [string]$WorkspaceRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$toolchain = 'D:\software\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi\bin'
$gcc = Join-Path $toolchain 'arm-none-eabi-gcc.exe'
$objcopy = Join-Path $toolchain 'arm-none-eabi-objcopy.exe'
$size = Join-Path $toolchain 'arm-none-eabi-size.exe'

foreach ($tool in @($gcc, $objcopy, $size)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "GNU Arm tool not found: $tool"
    }
}

$outputDir = Join-Path $WorkspaceRoot 'build\gcc'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$sources = @(
    'Project\GCC\startup_stm32h743xx.c',
    'App\main.c',
    'App\boot_jump.c',
    'App\gsa200.c',
    'App\j8_uart_test.c',
    'Bsp\LED\bsp_led.c',
    'Bsp\UART\bsp_uart.c',
    'Bsp\system_stm32h7xx.c',
    'Driver\Src\stm32h7xx_ll_dma.c',
    'Driver\Src\stm32h7xx_ll_gpio.c',
    'Driver\Src\stm32h7xx_ll_i2c.c',
    'Driver\Src\stm32h7xx_ll_pwr.c',
    'Driver\Src\stm32h7xx_ll_rcc.c',
    'Driver\Src\stm32h7xx_ll_spi.c',
    'Driver\Src\stm32h7xx_ll_usart.c',
    'Driver\Src\stm32h7xx_ll_utils.c'
) | ForEach-Object { Join-Path $WorkspaceRoot $_ }

$includes = @(
    'App',
    'Bsp',
    'Bsp\LED',
    'Bsp\UART',
    'CMSIS\Inc',
    'Driver\Inc',
    'Driver\Inc\Legacy',
    'STM32H7xx_Device\Inc'
) | ForEach-Object { '-I' + (Join-Path $WorkspaceRoot $_) }

$elf = Join-Path $outputDir 'servo_app.elf'
$map = Join-Path $outputDir 'servo_app.map'
$bin = Join-Path $outputDir 'servo_app.bin'
$hex = Join-Path $outputDir 'servo_app.hex'
$linkerScript = Join-Path $WorkspaceRoot 'Project\GCC\STM32H743VITX_FLASH.ld'

$arguments = @(
    '-std=gnu11',
    '-mcpu=cortex-m7',
    '-mthumb',
    '-mfpu=fpv5-d16',
    '-mfloat-abi=hard',
    '-O2',
    '-g3',
    '-ffunction-sections',
    '-fdata-sections',
    '-fno-common',
    '-Wall',
    '-Wextra',
    '-Wno-override-init',
    '-DSTM32H743xx',
    '-DUSE_FULL_LL_DRIVER',
    '-DRAM_DEBUG=0'
) + $includes + $sources + @(
    '-T', $linkerScript,
    '--specs=nano.specs',
    '--specs=nosys.specs',
    '-Wl,--gc-sections',
    "-Wl,-Map=$map,--cref",
    '-Wl,--start-group',
    '-lc',
    '-lm',
    '-Wl,--end-group',
    '-o', $elf
)

Write-Host 'Building STM32H743 Flash firmware with GNU Arm GCC...'
& $gcc @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $objcopy -O binary $elf $bin
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $objcopy -O ihex $elf $hex
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $size -A -x $elf
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build completed: $elf"
