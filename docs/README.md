# UNA Watch hardware reference

Part numbers come from the BOMs and schematics in `schematics/` (a symlink into
`refs/una-hardware`) and from the kernel driver sources in
`refs/una-kernel-public/Software/Libs/{Header,Source}/Hardware/`.

PDFs are not tracked in git. `./fetch-datasheets.sh` downloads them (skips files
that already exist) and holds the source URL for every file.

Official specifications page (blocks automated fetches, open in a browser):
<https://unawatch.zendesk.com/hc/en-gb/articles/5711102134047-UNA-Watch-Complete-Specifications>

## Boards

| Board | Schematic | Contents |
|---|---|---|
| UNAcore Rev3.2 | `schematics/UNAcore/` | MCU, eMMC, PMIC, fuel gauge, USB-C, haptics, buzzer |
| UNAlink_BLUENRG2 Rev1.4 | `schematics/UNAlink_BLUENRG2/` | BLE SoC, balun |
| UNAlink_AG3335M Rev1.4 | `schematics/UNAlink_AG3335M/` | GNSS, LNAs, IMU, magnetometer |
| UNAview_LS012 Rev1.3 | `schematics/UNAview_LS012/` | display FPC, level shifters, backlight |
| UNAbody_PAH8316 Rev1.0 | `schematics/UNAbody_PAH8316/` | PPG front end, LEDs, photodiodes |
| UNAaltimeter_MS5837 Rev2.0 | `schematics/UNAaltimeter_MS5837/` | barometer |

## Components

Status: ✔ present, – no public datasheet (see "Missing" below).

| Function | Part | File | |
|---|---|---|---|
| MCU | ST STM32U5A5QJI6Q (UFBGA132, Cortex-M33, 4 MB flash, 2.5 MB RAM) | `mcu/STM32U5A5xx-datasheet.pdf` | ✔ |
| | reference manual RM0456 | `mcu/RM0456-STM32U5-reference-manual.pdf` | ✔ |
| | Cortex-M33 programming manual PM0264 | `mcu/PM0264-Cortex-M33-programming-manual.pdf` | ✔ |
| | errata ES0553 (not ES0499, which is for U575/U585) | `mcu/ES0553-STM32U59x-U5Ax-errata.pdf` | ✔ |
| | hardware design AN5373 | `mcu/AN5373-STM32U5-hardware-getting-started.pdf` | ✔ |
| BLE | ST BlueNRG-234N (SPI network coprocessor) | `ble/BlueNRG-2N-datasheet.pdf`, `ble/BlueNRG-2-datasheet.pdf` | ✔ |
| | BLE stack programming PM0257 | `ble/PM0257-BlueNRG-BLE-stack-programming.pdf` | ✔ |
| | balun ST BALF-NRG-02D3 | `ble/BALF-NRG-02D3-balun.pdf` | ✔ |
| | 32 MHz crystal Murata XRCGB32M000F2P00R0 | | – |
| Display | Sharp LS012B7DD06A, 1.2" 240x240 64-colour memory-in-pixel, parallel RGB | `display/LS012B7DD06-specification.pdf` (spec LD-30101 for the part without the "A" suffix; scanned, not text-searchable) | ✔ |
| | level shifters 2x TI TXB0108 | `display/TXB0108-level-shifter.pdf` | ✔ |
| | backlight switch onsemi NTK3139P | `display/NTK3139P-backlight-switch.pdf` | ✔ |
| GNSS | Airoha AG3335MV/B (L1+L5) | | – |
| | LNAs Infineon BGA524N6 (L1), BGA855N6 (L5) | `gnss/BGA524N6-L1-LNA.pdf`, `gnss/BGA855N6-L5-LNA.pdf` | ✔ |
| | LDO ST LD57100 | `gnss/LD57100-LDO.pdf` | ✔ |
| | 32 kHz crystal ECS ECS-.327-6-16R | `gnss/ECX-16-32kHz-crystal.pdf` (series sheet) | ✔ |
| | TCXO Golledge GTXO-203T 26 MHz; Murata SAW filters | | – |
| IMU | Bosch BMI270 | `sensors/BMI270-imu.pdf` | ✔ |
| Magnetometer | Bosch BMM350 | `sensors/BMM350-magnetometer.pdf` | ✔ |
| Barometer | TE MS5837-02BA01 | `sensors/MS5837-02BA-barometer.pdf` | ✔ |
| Heart rate / SpO2 | PixArt PAH8316LS-IN | | – |
| | LEDs ams OSRAM SFH 7016 | `sensors/SFH7016-ppg-led.pdf` | ✔ |
| | photodiodes ams OSRAM SFH 2703 | `sensors/SFH2703-ppg-photodiode.pdf` | ✔ |
| | LDOs onsemi NCP110, NCP705 | `sensors/NCP110-LDO.pdf`, `sensors/NCP705-LDO.pdf` | ✔ |
| PMIC / charger | NXP PCA9420UK | `power/PCA9420-pmic.pdf` | ✔ |
| Fuel gauge | Maxim MAX17262 | `power/MAX17262-fuel-gauge.pdf` | ✔ |
| USB-C PD sink | ST STUSB4500L | `power/STUSB4500-usb-pd-sink.pdf` | ✔ |
| | P-FET ST STL4P3LLH6 | `power/STL4P3LLH6-pfet.pdf` | ✔ |
| | protection 2x NXP NX20P0407 | `power/NX20P0407-usb-c-protection.pdf` | ✔ |
| Boost 5 V | TI TPS61299 | `power/TPS61299-boost.pdf` | ✔ |
| LDOs | TI TPS7A02, Diodes AP7350D | `power/TPS7A02-LDO.pdf`, `power/AP7350-LDO.pdf` | ✔ |
| Load switches | onsemi NCP335 | `power/NCP334-NCP335-load-switch.pdf` | ✔ |
| Haptics | TI DRV2625 + Vybronics VZ30C1T8219732L ERM | `power/DRV2625-haptic-driver.pdf`, `power/VZ30C1T8219732L-vibration-motor.pdf` | ✔ |
| Buzzer driver | Diodes PAM8904E | `power/PAM8904E-piezo-driver.pdf` | ✔ |
| Storage | Kingston EMMC04G-CT32-01G10 (4 GB eMMC 5.1) | `memory/EMMC04G-CT32-emmc.pdf` | ✔ |
| ID EEPROMs | onsemi N24S64B (one per module board) | `memory/N24S64B-eeprom.pdf` | ✔ |
| 32 kHz LSE | SiTime SiT1532 MEMS oscillator | `misc/SiT1532-32kHz-oscillator.pdf` | ✔ |
| 16 MHz HSE | ECS ECS-160-8-37-CWY | `misc/ECX-1637-16MHz-crystal.pdf` (series sheet) | ✔ |
| Level shifters | Diodes 74LVC1T45, TI TXS0102 | `misc/74LVC1T45-level-shifter.pdf`, `misc/TXS0102-level-shifter.pdf` | ✔ |
| ESD | ST ESDA25P35-1U1M, Semtech RClamp2261ZA | `misc/ESDA25P35-1U1M-tvs.pdf` | ✔ |

## Missing

**No public datasheet**

- PixArt PAH8316LS and Airoha AG3335M: NDA only. The best reference is the kernel
  driver source (`Hardware/PAH8316LS/`, `Hardware/AG3335/`) and
  `refs/una-kernel-public/Software/ThirdParty/VendorLib/include/pixart/`.
- Murata crystal and SAW filters, Golledge TCXO, Semtech RClamp2261ZA: no direct
  PDF link found.

## Notes

- **Which MCU is fitted is ambiguous.** The Rev3.2 BOM says STM32U5A5QJI6Q (4 MB
  flash); a schematic ECN and the kernel's `.ioc` say STM32U595QII6Q (2 MB), and
  the kernel ships startup files for both. They share RM0456 and errata ES0553; the
  U595 has its own datasheet (DS13633), not downloaded. The kernel linker script used for release builds is
  `STM32U5A5QJIXQ_FLASH.ld`.
- **No touch controller is fitted.** TOUCH_INT/TOUCH_RST are routed to the display
  connector but nothing is populated; input is the four buttons.
- **No display controller IC.** The MCU clocks the panel directly through the
  TXB0108 level shifters (driver: `Hardware/LS012B7DD06A`).
- **Debug access** is through the USB-C connector in Debug Accessory Mode, not a
  header: SWDIO/SWCLK on the A2/A3 pair, reset and debug UART TX (USART3, PC4,
  921600 baud) on B10/B11. This pinout was read from extracted schematic text;
  check it against `schematics/UNAcore/` sheet 6 before wiring an adapter.
- USB in normal mode is a composite CDC-ACM + mass-storage device, `0483:52a4`.
- No LoRa, Wi-Fi or NFC on any board.
