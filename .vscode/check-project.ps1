param(
    [Parameter(Mandatory = $true)]
    [string]$WorkspaceRoot
)

$compiler = 'D:\software\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi\bin\arm-none-eabi-gcc.exe'
$projectDir = Join-Path $WorkspaceRoot 'Project\EWARM'
$projectFile = Join-Path $projectDir 'servo_app.ewp'

if (-not (Test-Path -LiteralPath $compiler)) {
    Write-Error "Compiler not found: $compiler"
    exit 1
}

if (-not (Test-Path -LiteralPath $projectFile)) {
    Write-Error "Project file not found: $projectFile"
    exit 1
}

[xml]$project = Get-Content -Raw -LiteralPath $projectFile
$sources = @(
    $project.SelectNodes('//file/name') |
        ForEach-Object { $_.InnerText } |
        Where-Object { $_ -like '*.c' } |
        ForEach-Object {
            $path = $_.Replace('$PROJ_DIR$', $projectDir)
            [System.IO.Path]::GetFullPath($path)
        }
)

$includePaths = @(
    'App',
    'Bsp',
    'Bsp\LED',
    'Bsp\UART',
    'CMSIS\Inc',
    'Driver\Inc',
    'Driver\Inc\Legacy',
    'STM32H7xx_Device\Inc'
)

$compilerArgs = @(
    '-std=c11',
    '-mcpu=cortex-m7',
    '-mthumb',
    '-fsyntax-only',
    '-w',
    '-DSTM32H743xx',
    '-DUSE_FULL_LL_DRIVER',
    '-DRAM_DEBUG=0'
)

foreach ($includePath in $includePaths) {
    $compilerArgs += '-I' + (Join-Path $WorkspaceRoot $includePath)
}

$failed = 0
foreach ($source in $sources) {
    $relativePath = $source.Substring($WorkspaceRoot.Length + 1)
    Write-Host "Checking $relativePath"
    & $compiler @compilerArgs $source

    if ($LASTEXITCODE -ne 0) {
        $failed++
        Write-Host "[FAILED] $relativePath" -ForegroundColor Red
    }
    else {
        Write-Host "[OK] $relativePath" -ForegroundColor Green
    }
}

if ($failed -ne 0) {
    Write-Host "Check failed: $failed of $($sources.Count) source files contain errors." -ForegroundColor Red
    exit 1
}

Write-Host "Check passed: all $($sources.Count) source files are error-free." -ForegroundColor Green
exit 0
