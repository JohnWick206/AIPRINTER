# Open serial monitor with ESP-IDF environment activated
# Usage: in D:\sticker\idf\net_speed run   .\mon.ps1
# Exit monitor: Ctrl + ]

param([string]$Port = "COM4")

$env:IDF_TOOLS_PATH      = "D:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\tools\python_env\idf5.5_py3.11_env"

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. "D:\Espressif\esp-idf\export.ps1" | Out-Null

Set-Location $PSScriptRoot

Write-Host ""
Write-Host ("Serial monitor on {0} ..." -f $Port) -ForegroundColor Cyan
Write-Host "  - board resets automatically and runs one default image first" -ForegroundColor Gray
Write-Host "  - after the prompt line appears, type and press Enter" -ForegroundColor Gray
Write-Host "  - exit: Ctrl + ]" -ForegroundColor Gray
Write-Host ""

idf.py -p $Port monitor
