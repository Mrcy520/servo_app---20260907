$ErrorActionPreference = 'Stop'
$ToolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python = Join-Path $ToolRoot '.venv\Scripts\python.exe'

if (-not (Test-Path -LiteralPath $Python)) {
    throw 'Please run .\run.ps1 -Install first.'
}

& $Python -m pip install -r (Join-Path $ToolRoot 'requirements-dev.txt')
Push-Location $ToolRoot
try {
    & $Python -m PyInstaller --noconfirm --clean --windowed --name ServoPilot `
        --collect-all PySide6 main.py
}
finally {
    Pop-Location
}

Write-Host "Build output: $(Join-Path $ToolRoot 'dist\ServoPilot')"

