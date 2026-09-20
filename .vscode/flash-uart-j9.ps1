param(
    [Parameter(Mandatory = $false)]
    [string]$WorkspaceRoot = (Split-Path -Parent $PSScriptRoot),

    [Parameter(Mandatory = $false)]
    [string]$Port = 'COM3',

    [Parameter(Mandatory = $false)]
    [switch]$SkipTrigger
)

$ErrorActionPreference = 'Stop'

$hex = Join-Path $WorkspaceRoot 'build\gcc\servo_app.hex'
if (-not (Test-Path -LiteralPath $hex)) {
    throw "Firmware not found: $hex. Run the GCC build task first."
}

$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($Port -notin $availablePorts) {
    throw "Serial port $Port was not found. Available ports: $($availablePorts -join ', ')"
}

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

if (-not $SkipTrigger) {
    $serial = [System.IO.Ports.SerialPort]::new(
        $Port,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )
    $serial.Handshake = [System.IO.Ports.Handshake]::None
    $serial.ReadTimeout = 100
    $serial.WriteTimeout = 1000
    $triggerByte = [byte[]](0x7F)
    $ackReceived = $false

    try {
        Write-Host "Triggering the application boot jump on $Port (115200 8N1)..."
        $serial.Open()
        $serial.DiscardInBuffer()
        $serial.DiscardOutBuffer()
        for ($index = 0; $index -lt 3; $index++) {
            $serial.Write($triggerByte, 0, $triggerByte.Length)
            $serial.BaseStream.Flush()
            if ($index -lt 2) { Start-Sleep -Milliseconds 30 }
        }

        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        while ([DateTime]::UtcNow -lt $deadline) {
            try {
                $value = $serial.ReadByte()
                if ($value -eq 0x79) {
                    $ackReceived = $true
                    break
                }
            }
            catch [System.TimeoutException] {
                # Continue polling until the overall deadline expires.
            }
        }
    }
    finally {
        if ($serial.IsOpen) { $serial.Close() }
        $serial.Dispose()
    }

    if (-not $ackReceived) {
        throw "No 0x79 ACK received from the application on $Port. Check that the board is running, J9 uses a four-wire full-duplex RS-422 adapter, and TX/RX differential pairs are crossed correctly."
    }

    Write-Host 'Application ACK received; STM32H743 entered the ROM bootloader.'
    Start-Sleep -Milliseconds 300
}

Write-Host "Programming $hex through the ROM bootloader on $Port (115200 8E1)..."
$connectionArguments = @(
    '-c',
    "port=$Port",
    'br=115200',
    'P=EVEN',
    'db=8',
    'sb=1',
    'fc=OFF'
)

& $programmer @connectionArguments -w $hex -v -g 0x08000000
if ($LASTEXITCODE -ne 0) {
    throw "UART bootloader programming failed with exit code $LASTEXITCODE."
}

Write-Host 'UART programming verified; application started at 0x08000000.'
