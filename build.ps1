# Build (and optionally flash) this ESP-IDF project. ASCII only on purpose:
# PowerShell 5.1 parses .ps1 as GBK, so any UTF-8 Chinese here would break the
# quoting and kill the script with TerminatorExpectedAtEndOfString.
#
# Why not `idf.py build`:
#   On this machine it reliably stalls at "[3/9] Completed 'bootloader'" with a
#   ninja.exe that has 0% CPU and no children. Calling ninja directly in the
#   build dir finishes in seconds. So: ninja builds, idf.py only flashes.
#
# Why CCACHE_DISABLE:
#   ccache lives on C:\Users\<user>\AppData\Local\ccache. Once a build is killed
#   mid-flight its lock is left behind and every later build blocks on it.
#
# Usage:
#   .\build.ps1            # build only
#   .\build.ps1 -Flash     # build, then flash to COM4
#   .\build.ps1 -Flash -Port COM5

param(
    [switch]$Flash,
    [string]$Port = "COM4"
)

$ErrorActionPreference = "Continue"
try { chcp 65001 > $null } catch { }

$env:IDF_TOOLS_PATH      = "D:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\tools\python_env\idf5.5_py3.11_env"
$env:CCACHE_DISABLE      = "1"

$root = $PSScriptRoot
if (-not $root) { $root = "D:\sticker\idf\net_speed" }

Write-Output "== env =="
. "D:\Espressif\esp-idf\export.ps1" | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Output "ENV_FAILED"; exit 1 }

Write-Output "== build (ninja) =="
Set-Location (Join-Path $root "build")
ninja
if ($LASTEXITCODE -ne 0) {
    Write-Output "BUILD_FAILED"
    exit 1
}

$bin = Join-Path $root "build\net_speed.bin"
if (Test-Path $bin) {
    $f = Get-Item $bin
    Write-Output ("BUILD_OK  {0}  {1:N1} KB  {2}" -f "net_speed.bin", ($f.Length / 1KB), $f.LastWriteTime.ToString("HH:mm:ss"))
} else {
    Write-Output "BUILD_OK (no bin found)"
}

if (-not $Flash) { exit 0 }

Write-Output "== port check =="
try {
    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
    $sp.Open()
    $sp.Close()
} catch {
    Write-Output ("PORT_BUSY  {0} is held by another program - close the serial monitor first" -f $Port)
    exit 3
}

Write-Output ("== flash {0} ==" -f $Port)
Set-Location $root
& idf.py -p $Port flash
if ($LASTEXITCODE -ne 0) {
    Write-Output "FLASH_FAILED"
    exit 2
}

# The face atlas lives in its own partition, so it is never compiled into the app.
# Only rewrite it when the file changed since the last flash.
$atlas = Join-Path $root "main\grokbot.atlas"
$stamp = Join-Path $root "buildtlas.flashed"
$needAtlas = $true
if ((Test-Path $atlas) -and (Test-Path $stamp)) {
    if ((Get-Item $atlas).LastWriteTimeUtc -le (Get-Item $stamp).LastWriteTimeUtc) { $needAtlas = $false }
}
if ($needAtlas) {
    if (-not (Test-Path $atlas)) {
        Write-Output "ATLAS_MISSING  main\grokbot.atlas not found"
        exit 4
    }
    Write-Output "== flash atlas (6 MB partition) =="
    & python -m esptool --chip esp32s3 -p $Port -b 460800 --before default_reset --after hard_reset write_flash 0x410000 $atlas
    if ($LASTEXITCODE -ne 0) {
        Write-Output "ATLAS_FLASH_FAILED"
        exit 4
    }
    (Get-Item $atlas).LastWriteTimeUtc | Out-File -FilePath $stamp -Encoding ascii
} else {
    Write-Output "ATLAS_SKIP  unchanged since last flash"
}
Write-Output "FLASH_OK"
exit 0
