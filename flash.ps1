# 烧录 ESP32-S3（手动下载模式）
#
# 用法：
#   1. 按住 BOOT 键，点一下 RST，松开 BOOT（进入下载模式）
#   2. 运行：powershell -ExecutionPolicy Bypass -File flash.ps1
#
# 为什么需要这个脚本：
#   ESP32-S3 的 USB-Serial/JTAG 口在板子反复重启（固件崩溃）时，
#   esptool 的自动复位抓不到握手窗口，会报 "Write timeout"。
#   手动进下载模式 + --before no_reset 就绕过了这个问题。

param(
    [string]$Port = "COM10",
    [string]$BuildDir = "$PSScriptRoot\firmware\.pio\build\esp32s3"
)

$ErrorActionPreference = "Stop"

$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"

if (-not (Test-Path $esptool)) {
    Write-Host "找不到 esptool: $esptool" -ForegroundColor Red
    Write-Host "先跑一次 pio run 生成工具链" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path "$BuildDir\firmware.bin")) {
    Write-Host "找不到 firmware.bin，先编译：" -ForegroundColor Red
    Write-Host "  pio run" -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "==================================" -ForegroundColor Cyan
Write-Host " 烧录到 $Port" -ForegroundColor Cyan
Write-Host "==================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "如果下面报 Write timeout：" -ForegroundColor Yellow
Write-Host "  1. 按住板子上的 BOOT 键" -ForegroundColor Yellow
Write-Host "  2. 点一下 RST" -ForegroundColor Yellow
Write-Host "  3. 松开 BOOT" -ForegroundColor Yellow
Write-Host "  4. 重新运行本脚本" -ForegroundColor Yellow
Write-Host ""

$args = @(
    $esptool
    "--port", $Port
    "--baud", "921600"
    "--before", "no_reset"
    "--after", "hard_reset"
    "--chip", "esp32s3"
    "write_flash", "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"
    "0x0",     "$BuildDir\bootloader.bin"
    "0x8000",  "$BuildDir\partitions.bin"
    "0x10000", "$BuildDir\firmware.bin"
)

& $py @args 2>&1 | ForEach-Object { $_ }

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "烧录成功" -ForegroundColor Green
    Write-Host ""
    Write-Host "查看串口输出：" -ForegroundColor Cyan
    Write-Host "  pio device monitor --port $Port --baud 115200" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "烧录失败。" -ForegroundColor Red
    Write-Host "手动进下载模式后再试：" -ForegroundColor Yellow
    Write-Host "  按住 BOOT → 点 RST → 松开 BOOT" -ForegroundColor Yellow
}
