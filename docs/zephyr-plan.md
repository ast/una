# UNA Watch on Zephyr: from test screen to daily watch

## Context

Zephyr 4.5 runs on the watch behind the stock UNA bootloader, with MCUboot, a
working display driver, LVGL, a USB shell, screenshots, simulated buttons and
hands-free flashing (`una-z flash`, ~30 s, automatic rollback). Nothing else of
the hardware is driven yet: no power management, charger setup, storage,
sensors, Bluetooth or GPS, and the UI is a test page.

Goal: a watch that is good to wear daily with roughly the stock feature set,
built so that an LLM can develop and verify it over USB. Decisions taken:

- **Phone:** Android, standard services where they exist. Android has no
  built-in notification forwarding (no ANCS equivalent), so the open-source
  Gadgetbridge app is the companion; standard GATT services (time, battery,
  device info, alert notification, immediate alert, heart rate) are used
  wherever Gadgetbridge accepts them.
- **Apps:** built into the firmware behind one small app interface, shaped so
  they can later be loaded from eMMC as Zephyr extensions (LLEXT). Stock
  `.uapp` files are not supported.
- **Heart rate:** link PixArt's closed library from the local kernel clone at
  build time (never copied into this repo); an open driver may follow.
- **Priorities:** all four areas (notifications/media, GPS workouts, daily
  health, utilities), after the basics.

Constraints that hold throughout: never write the first 384 KB of flash; keep
the eMMC's stock layout and never format `UNA BOOT`, so the stock firmware can
be restored at any time; every feature gets a shell hook for inspection and
injection.

## Facts the plan rests on (from the stock source)

- Buses: I2C1 PB6/PB7 (PCA9420 0x61, MAX17262 0x36, STUSB4500 0x29, DRV2625
  0x5A); I2C2 PB13/PB14 (PAH8316 0x40, MS5837 0x76); I2C4 PD12/PD13 (BMI270
  0x68 INT PA1, BMM350 0x14 INT PC2); SPI1 PG2/PG3/PB5 + CS PA15, IRQ PA9,
  RESET PA8 (BlueNRG-2N); LPUART1 PA2/PA3 921600 (AG3335, enable PG5);
  SDMMC1 4-bit PC8-12/PD2, enable PD10. PG pins need VddIO2 enabled.
- PMIC values to reproduce: SW1 1.2 V, SW2 1.8 V, LDO1 1.8 V, LDO2 3.2 V,
  charge 150 mA to 4.40 V, top-off 10 mA; power-off = mode 2 via TOP_CNTL3;
  16 s long-press. Fuel gauge: EZ model, 200 mAh, VEmpty 3.4 V, 10 mOhm.
- Stock idle is about 70-80 uA in STOP2 with the RTC wake-up timer; battery is
  about 200 mAh, so power management decides whether this is wearable.
- eMMC regions (512-byte sectors): BOOT 0 +204800, SYS 204800 +204800, raw
  409600 +204800, USER 614400 to end; each exFAT behind its own MBR.
- BlueNRG-2N runs ST's full stack internally; Zephyr's `st,hci-spi-v2` driver
  with `req-ll-only` switches it to controller-only at runtime (template:
  `boards/shields/x_nucleo_bnrg2a1`). Untested on a 2N: this is the plan's
  largest technical risk and is tested first.
- Zephyr has drivers for BMI270 (no step counter/wrist-wake), BMM350, MS5837,
  MAX17262 (sensor API), PCA9420 (regulator only, no charger), eMMC via
  `sdmmc_stm32` + `CONFIG_SDMMC_STM32_EMMC`, exFAT, USB MSC, RTC, PWM buzzer,
  STM32U5 STOP modes. None for DRV2625 or AG3335 (the Quectel LCx6G GNSS
  driver speaks the same `$PAIR` commands and is the base).
- Open and reusable from the stock kernel/SDK (MIT): daily-health algorithms,
  floor counter, compass calibration, cadence estimator, FIT encoder
  (`una-sdk/Libs/Source/Fit`), EPO parser, DRV2625 and Bosch sensor setup.

## Phases

Each phase ends with the watch flashed, checked over USB, and a commit.

### 0. Risk spike: Bluetooth controller (half a day, before anything else)
Add SPI1 and a `st,hci-spi-v2` node to `una_watch.dts`, enable VddIO2 and the
Bluetooth shell, and check `bt init` / advertise / connect from the laptop.
Confirm that a reset of the chip returns it to its stock behaviour (the
LL-only switch should not persist). If it fails, Bluetooth is re-planned
(fallback: drive ST's on-chip stack through its ACI commands with an own thin
layer) before the UI is designed around it.

### 1. Power, so it can be worn
- I2C1 with bus-recovery wait; `nxp,pca9420` regulator nodes with the stock
  voltages; a small `una_pmic` module for what the driver lacks (charger
  registers, VIN limit, long-press time, interrupts on PG7, power-off, ship
  mode).
- `maxim,max17262` with the stock model values; battery percentage, voltage
  and current over shell and in the UI; low-battery power-off.
- Power management: `CONFIG_PM` with STOP2, tick from LPTIM/RTC on the LSE,
  USB attach holds the system awake, display polarity timers keep running in
  STOP2. Target: within 2x of the stock idle current, measured with the fuel
  gauge's current register over hours.
- Charging and USB-attach handling (STUSB4500 alert, NX20P0407 fault).
- Shell: `power off|status`, `battery`.

### 2. Board services
- RTC time in UTC with time zone; `date` shell command.
- Buttons: SW1-SW4 named UP/SELECT/DOWN/BACK as stock, long-press and hold
  through `zephyr,input-longpress`.
- Backlight with timeout; buzzer (`pwm-buzzer` on TIM4_CH3, two enable pins).
- Haptics: new `ti,drv2625` driver for Zephyr's haptics API in
  `zephyr/una/drivers/haptics/`, ported from `Hardware/DRV2625.cpp`.
- Settings in internal flash (`storage_partition`, Zephyr settings + ZMS), so
  they survive without the eMMC.
- eMMC: SDMMC1 with `EMMC_EN` as a regulator and idle power-off; a disk layer
  that exposes the stock regions; exFAT mount of USER (and BOOT read-only);
  USB mass storage of USER on request (`usb drive on`), filesystem unmounted
  meanwhile.

### 3. UI shell and app framework
- App interface in `zephyr/una/app/src/apps/`: four kinds as in stock (watch
  face, glance, activity, utility); each app is a descriptor (id, name, icon,
  kind) plus lifecycle callbacks and an optional background service. Apps
  talk to the system only through zbus channels and a small C API; no direct
  driver access. That boundary is what LLEXT later exports.
- zbus channels for everything observable: buttons, time, battery, steps,
  heart rate, GPS fix, notifications. Shell commands publish to the same
  channels, which gives sensor simulation for free (`sim hr 142`,
  `sim gps 59.33 18.06`).
- Navigation as stock: home (face), glances list, top menu, activity menu,
  settings. LVGL theme and fonts tuned for 64 colours and four buttons.
- Host build of the same UI (`native_sim` with SDL) for fast iteration, plus
  a screenshot-comparison test script built on `una-z shot` / `press`.
- First apps: one digital and one analogue face, battery glance, settings
  (clock format, backlight, sounds, vibration, device info, reset, power off).

### 4. Sensors and daily health
- BMI270 on I2C4: accel/gyro through Zephyr's driver; step counter, activity
  class and wrist-wear wake-up added using Bosch's feature config (as
  `Hardware/BMI270/BMI270.cpp` does). Lift-to-wake drives the backlight.
- MS5837 and BMM350 through Zephyr's drivers; altitude, floors and compass
  ported from the stock kernel.
- Heart rate: Kconfig option `UNA_PIXART_HR` that links
  `refs/una-kernel-public/Software/ThirdParty/VendorLib/lib/libuna_vendor.a`
  when present and publishes HR and worn state; off by default so a clean
  clone builds. Check ABI (hard float, newlib symbols) as the first step.
- Daily health service: steps, floors, active minutes, average and resting
  HR, per-minute HR, stored per day on the eMMC in the stock JSON layout.
- Glances: steps, floors, HR, active minutes.

### 5. Bluetooth and phone
- Zephyr host on the MCU; identity address derived from the MCU UID; passkey
  pairing shown on the watch; bonds in settings.
- Standard services: Battery, Device Information, Current Time, Heart Rate,
  Alert Notification, Immediate Alert (find watch).
- Gadgetbridge link for notifications with text, media control, find phone
  and weather. Which Gadgetbridge device protocol to speak (InfiniTime-style
  GATT or the Bangle.js JSON protocol that the Zephyr-based ZSWatch uses) is
  decided at the start of this phase by reading Gadgetbridge's source; the
  watch-side code sits behind one `phone` module either way.
- mcumgr over Bluetooth, so firmware can also be updated without the cable.
- Notification list and detail screens, media control screen.

### 6. GPS and workouts
- `airoha,ag3335m` GNSS driver derived from `gnss_quectel_lcx6g.c`: enable
  pin PG5, 921600 baud, the stock power-on sequence, EPO injection
  (`PAIR471/590/600`) from `GPS_EPO/*.DAT` on the eMMC.
- Workout engine and activity apps (Run, Bike, Hike, Walk, Treadmill): timer,
  distance, pace, laps, HR, altitude; FIT file plus JSON summary written in
  the stock directory layout, using the SDK's FIT encoder.
- Getting data off: USB drive first; phone sync through Gadgetbridge if its
  protocol allows, otherwise later. EPO files are copied over USB until then.
- Satellite status screen; GPS simulation through the zbus channel.

### 7. Utilities
Alarm, timer, stopwatch, compass with calibration, sleep-mode schedule.

### 8. Loadable apps (later)
Enable LLEXT, export the app C API and LVGL, load `.llext` files from
`Apps/` on the eMMC, and move one built-in app out as the proof.

## Files

- `zephyr/una/boards/una/una_watch/una_watch.dts`, `_defconfig`: buses,
  regulators, sensors, eMMC, Bluetooth, GNSS, PM states.
- `zephyr/una/drivers/`: `haptics/drv2625.c`, `gnss/ag3335m.c`, PMIC charger
  helper, eMMC region disk; bindings under `zephyr/una/dts/bindings/`.
- `zephyr/una/app/src/`: `power/`, `ui/`, `apps/`, `health/`, `phone/`,
  `workout/`, `sim/` (shell injection); `prj.conf` split into Kconfig
  fragments per area.
- `zephyr/una/west.yml`: add `hal_st`/Bosch and Bluetooth-related modules as
  needed.
- `scripts/una-z`: `test` (scripted press/shot sequences), `log`, faster
  upload; `README.md` kept current.

## Verification

- Every phase: `west build -d build/zephyr && una-z flash`, then shell
  checks and `una-z shot` screenshots compared against expectations.
- Power: idle current from the fuel gauge over several hours on battery;
  charge to full on the cable; power off and on by button; 20 s long-press
  still reaches the UNA bootloader.
- Storage: files written by Zephyr are readable on the laptop over USB and by
  the stock firmware after restoring `firmware/official-v1.7.0`.
- Sensors: live values over shell against reality (steps walked, floors,
  pulse against a manual count); simulated values drive the same screens.
- Bluetooth: pair with the Android phone, time sync, a test notification,
  media control; laptop-side check with `bluetoothctl`.
- GPS: outdoor fix time with and without EPO; a recorded FIT file opens in a
  FIT viewer.
- Regression: restore-to-stock is exercised once after phases 1 and 2.
