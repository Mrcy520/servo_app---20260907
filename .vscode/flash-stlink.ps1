param(
    [Parameter(Mandatory = $false)]
    [string]$WorkspaceRoot = (Split-Path -Parent $PSScriptRoot),

    [Parameter(Mandatory = $false)]
    [switch]$ProbeOnly,

    [Parameter(Mandatory = $false)]
    [string]$SerialNumber = '22025702234D4E504E413836'
)

$ErrorActionPreference = 'Stop'

$bundleRoot = Join-Path $env:LOCALAPPDATA 'stm32cube\bundles\programmer'
$programmer = Get-ChildItem -Path $bundleRoot -Filter 'STM32_Programmer_CLI.exe' -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $programmer) {
    $command = Get-Command 'STM32_Programmer_CLI.exe' -ErrorAction SilentlyContinue
    if ($command) { $programmer = $command.Source }
}
if (-not $programmer) {
    throw 'STM32CubeProgrammer CLI was not found. Install the STM32CubeIDE for Visual Studio Code programmer bundle.'
}

$connectionArguments = @(
    '-c',
    'port=SWD',
    "sn=$SerialNumber",
    'mode=UR',
    'reset=HWrst',
    'freq=1000'
)

if ($ProbeOnly) {
    Write-Host 'Testing the ST-Link and STM32H743 connection...'
    & $programmer @connectionArguments
    exit $LASTEXITCODE
}

$elf = Join-Path $WorkspaceRoot 'build\gcc\servo_app.elf'
if (-not (Test-Path -LiteralPath $elf)) {
    throw "Firmware not found: $elf. Run the build task first."
}

Write-Host 'Programming STM32H743 through ST-Link...'
& $programmer @connectionArguments -w $elf -v -rst
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host 'Programming verified; target reset and started.'
