# Flash the Slitscan firmware over USB DFU.
# Put the board in bootloader mode first: hold BOOT, tap RST, release BOOT.
# Usage:  .\tools\flash.ps1            (flashes firmware\build\slitscan.hex)
#         .\tools\flash.ps1 other.hex
#         .\tools\flash.ps1 testdata\firmware_dump.bin   (restore the original firmware)
param([string]$Hex = "$PSScriptRoot\..\firmware\build\slitscan.hex")

$cli = Get-Command STM32_Programmer_CLI -ErrorAction SilentlyContinue
if ($cli) { $cli = $cli.Source }
else { $cli = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' }
if (-not (Test-Path $cli)) { Write-Error "STM32_Programmer_CLI not found; install STM32CubeProgrammer"; exit 1 }
if (-not (Test-Path $Hex)) { Write-Error "Firmware file not found: $Hex (build first)"; exit 1 }

Write-Host "Flashing $Hex ..."
# .bin files carry no address, so they need the flash start address.
$writeArgs = @('-w', $Hex)
if ($Hex -like '*.bin') { $writeArgs += '0x08000000' }
# USB DFU can't reset the chip (-rst needs SWD), so ask the bootloader to jump to the new program.
& $cli -c port=usb1 @writeArgs -v -s 0x08000000
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Flashing failed. Is the board in bootloader mode (hold BOOT, tap RST, release BOOT)?" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "Done. If the new firmware doesn't start, tap RST." -ForegroundColor Green
