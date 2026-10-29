$ErrorActionPreference = "Stop"

Set-Location $PSScriptRoot
python -m pip install -r requirements.txt
python main.py

