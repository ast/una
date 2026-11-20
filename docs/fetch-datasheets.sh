#!/usr/bin/env bash
# Download the datasheets listed in README.md into this directory.
# PDFs are gitignored; rerun this to restore them. Existing files are skipped.
set -u
cd "$(dirname "$0")"

UA='Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
fail=0

get() { # get <dir/file.pdf> <url>
  local out=$1 url=$2
  [ -s "$out" ] && return 0
  mkdir -p "$(dirname "$out")"
  # st.com and analog.com reset the stream without browser-like headers
  if curl -fsSL --http2 --max-time 180 -A "$UA" -H 'Accept: */*' -H 'Accept-Language: en-US,en;q=0.5' \
       -o "$out.part" "$url" && [ "$(head -c 5 "$out.part")" = "%PDF-" ]; then
    mv "$out.part" "$out"
    echo "ok    $out"
  else
    rm -f "$out.part"
    echo "FAIL  $out  <- $url"
    fail=1
  fi
}

ST=https://www.st.com/resource/en
TI=https://www.ti.com/lit/ds/symlink
ON=https://www.onsemi.com/download/data-sheet/pdf
DI=https://www.diodes.com/assets/Datasheets

# MCU: STM32U5A5QJI6Q (STM32U595QII6Q on older boards; same documents)
get mcu/STM32U5A5xx-datasheet.pdf            $ST/datasheet/stm32u5a5qj.pdf
get mcu/RM0456-STM32U5-reference-manual.pdf  $ST/reference_manual/rm0456-stm32u5-series-armbased-32bit-mcus-stmicroelectronics.pdf
get mcu/PM0264-Cortex-M33-programming-manual.pdf $ST/programming_manual/pm0264-stm32-armcortex-m33-mcus-programming-manual-stmicroelectronics.pdf
get mcu/ES0553-STM32U59x-U5Ax-errata.pdf     $ST/errata_sheet/es0553-stm32u59xxx-and-stm32u5axxx-device-errata-stmicroelectronics.pdf
get mcu/AN5373-STM32U5-hardware-getting-started.pdf $ST/application_note/an5373-getting-started-with-stm32u5-series-hardware-development-stmicroelectronics.pdf

# BLE: BlueNRG-234N + balun
get ble/BlueNRG-2N-datasheet.pdf             $ST/datasheet/bluenrg-2n.pdf
get ble/BlueNRG-2-datasheet.pdf              $ST/datasheet/bluenrg-2.pdf
get ble/PM0257-BlueNRG-BLE-stack-programming.pdf $ST/programming_manual/pm0257-bluenrg1-bluenrg2-ble-stack-v2x-programming-guidelines-stmicroelectronics.pdf
get ble/BALF-NRG-02D3-balun.pdf              $ST/datasheet/balf-nrg-02d3.pdf

# Display: Sharp LS012B7DD06A (no controller IC; driven through level shifters)
get display/LS012B7DD06-specification.pdf    https://www.data-modul.com/sites/default/files/products/LS012B7DD06-specification-12038622.pdf
get display/TXB0108-level-shifter.pdf        $TI/txb0108.pdf
get display/NTK3139P-backlight-switch.pdf    $ON/ntk3139p-d.pdf

# GNSS: Airoha AG3335MV/B (no public datasheet) front end and clocks
get gnss/BGA524N6-L1-LNA.pdf                 https://static.chipdip.ru/lib/618/DOC046618784.pdf
get gnss/BGA855N6-L5-LNA.pdf                 https://static.chipdip.ru/lib/637/DOC011637188.pdf
get gnss/LD57100-LDO.pdf                     $ST/datasheet/ld57100.pdf
get gnss/ECX-16-32kHz-crystal.pdf            https://ecsxtal.com/store/pdf/ECX-16.pdf

# Sensors
get sensors/BMI270-imu.pdf                   https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf
get sensors/BMM350-magnetometer.pdf          https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmm350-ds001.pdf
get sensors/MS5837-02BA-barometer.pdf        https://www.mouser.lt/datasheet/3/80/44/ENG_DS_MS5837_02BA01_A11.pdf
get sensors/SFH7016-ppg-led.pdf              https://look.ams-osram.com/m/566c673078988944/original/SFH-7016.pdf
get sensors/SFH2703-ppg-photodiode.pdf       'https://www.mouser.com/catalog/specsheets/ams%20OSRAM_11-04-2025_SFH%202703_EN.pdf'
get sensors/NCP110-LDO.pdf                   $ON/ncp110-d.pdf
get sensors/NCP705-LDO.pdf                   $ON/ncp705-d.pdf

# Power, USB-C, haptics, buzzer
get power/PCA9420-pmic.pdf                   https://www.nxp.com/docs/en/data-sheet/PCA9420.pdf
get power/MAX17262-fuel-gauge.pdf            https://www.analog.com/media/en/technical-documentation/data-sheets/MAX17262.pdf
get power/STUSB4500-usb-pd-sink.pdf          $ST/datasheet/stusb4500.pdf
get power/STL4P3LLH6-pfet.pdf                $ST/datasheet/stl4p3llh6.pdf
get power/NX20P0407-usb-c-protection.pdf     https://www.nxp.com/docs/en/data-sheet/NX20P0407.pdf
get power/TPS61299-boost.pdf                 $TI/tps61299.pdf
get power/TPS7A02-LDO.pdf                    $TI/tps7a02.pdf
get power/AP7350-LDO.pdf                     $DI/AP7350.pdf
get power/NCP334-NCP335-load-switch.pdf      $ON/ncp334-d.pdf
get power/DRV2625-haptic-driver.pdf          $TI/drv2625.pdf
get power/VZ30C1T8219732L-vibration-motor.pdf https://www.vybronics.com/wp-content/uploads/datasheet-files/Vybronics-VZ30C1T8219732L-datasheet.pdf
get power/PAM8904E-piezo-driver.pdf          $DI/PAM8904E.pdf

# Memory
get memory/EMMC04G-CT32-emmc.pdf             https://www1.futureelectronics.com/doc/Kingston/EMMC04G-CT32-01G10.pdf
get memory/N24S64B-eeprom.pdf                $ON/n24s64b-d.pdf

# Clocks, level shifters, protection
get misc/SiT1532-32kHz-oscillator.pdf        https://www.sitime.com/datasheet/SiT1532
get misc/ECX-1637-16MHz-crystal.pdf          https://ecsxtal.com/store/pdf/ecx-1637.pdf
get misc/74LVC1T45-level-shifter.pdf         $DI/74LVC1T45.pdf
get misc/TXS0102-level-shifter.pdf           $TI/txs0102.pdf
get misc/ESDA25P35-1U1M-tvs.pdf              $ST/datasheet/esda25p35-1u1m.pdf

exit $fail
