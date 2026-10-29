param(
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$ToolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$VenvRoot = Join-Path $ToolRoot '.venv'
$Python = Join-Path $VenvRoot 'Scripts\python.exe'

if (-not (Test-Path -LiteralPath $Python)) {
    Write-Host 'Creating ServoPilot virtual environment...'
    py -3 -m venv $VenvRoot
    $Install = $true
}

if (-not $Install) {
    & $Python -c 'import PySide6, serial' 2>$null
    if ($LASTEXITCODE -ne 0) {
        $Install = $true
    }
}

if ($Install) {
    & $Python -m pip install --upgrade pip
    & $Python -m pip install -r (Join-Path $ToolRoot 'requirements.txt')
}

Push-Location $ToolRoot
try {
    & $Python main.py
}
finally {
    Pop-Location
}

