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
  (`una-kernel-public/SDK/Libs/Source/Fit`), EPO parser, DRV2625 and Bosch sensor setup.

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

**Open: idle current (deferred, usability features come first).** Measured
at v0.1.0 over 7 h on the wrist: 0.84 mA with the backlight off, 1.1 mA
overall, so about a week on the 200 mAh cell. The target above is about
160 uA. The CPU is awake under 0.1 % of the time, so this is a standing draw
during Stop, not CPU time. To do, time-boxed to a day or two:
- A shell switch per suspect, so each can be turned off on a running watch.
- Measure each for 20-30 minutes on battery; the gauge needs that long to
  resolve about 0.1 mA.
- Suspects, none checked yet: eMMC powered between the once-a-minute syncs;
  sensors and the GPS chip left in their default state; PMIC mode and
  regulators during Stop; whatever wakes the CPU twice a second (about 120
  Stop entries a minute when idle).

### Status after v0.1.0 (20 Nov 2026)

Done ahead of the phases below, to make the watch usable: named buttons with
long-press, a screen stack with menu and settings, stopwatch, timer, one
daily alarm, buzzer and vibration motor (register-level, in the app), and the
`native_sim` build with `una-z sim`. It is flashed and the screens work on
the watch; that the buzzer and motor are heard and felt is still to be
confirmed. Next: Bluetooth time and notifications through Gadgetbridge
(phase 5).

### Status after v0.3.0 (21 Nov 2026)

The navigation is no longer "as stock" (phase 3): the pages lie in one ring
with no menu, and the buttons mean the same everywhere (README, Buttons and
screens). Activities are not apps either (phase 6): there is to be one
recorder, started with one press, that writes a FIT file whatever the
activity; what it was is a label chosen afterwards. System pages show the
subsystems as an aircraft's displays do, and get a page per subsystem as
they arrive (GNSS, sensors, radio).

Done since: the recorder (FIT writer, emulated GNSS in the simulator; on
the watch it records time only), its files checked with a third-party FIT
decoder. The GNSS chip answers (own driver `airoha,ag3335m` after all:
Zephyr's LC76G driver starts the chip at boot and does not lock its sleep,
after which it stops answering); a fix outdoors is still to be seen.
Order from here: the GNSS chip on the watch (LPUART1, 921600 baud, power on PG5, Zephyr's
`quectel,lc76g` driver), then altitude gain from the MS5837.

The gauge reported 99-100 % through 22.3 h on battery on 20-21 Nov (22.5 mAh
drawn, so about 89 % was right). Its registers on the charger: MixSOC and
VFSOC at 108 %, RepSOC clamped to 100: the EZ model is full at a lower
voltage than the 4.40 V this cell is charged to, and at 1 mA the gauge
follows its voltage model. The percentage shown is now MixCap against
MixCap when the charger last reported full (`charge()` in `src/power.c`).
Still to be confirmed by a day of wear: the telemetry line carries `soc`,
`cap`, `rep` and `qh` for that.

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
### Status after v0.4.0 (22 Nov 2026)

The GPS receiver gets a first fix in about a minute outdoors (the SoC has
to stay out of Stop mode from before the first command to it).

Bluetooth did not go as phase 0 concluded. The BlueNRG-2N accepts the switch
to controller-only mode and connects, but its factory firmware (DTM 3.2.0,
stack 2.1c) passes no data either way: ST's DTM of that generation builds
the ACL path only with `LL_ONLY`, and this image is not built so. The spike
had only checked that a laptop connects. Zephyr's host is therefore not
used. The watch drives the chip's own stack with ST's ACI commands, as the
stock kernel does, through a layer of its own (`src/bluenrg.c`,
`src/bluetooth.c`); ST's X-CUBE-BLE2 sources gave the command layouts.
Phase 5 changes with it: no Zephyr GATT services and no mcumgr over
Bluetooth; the phone is spoken to in Gadgetbridge's Bangle.js protocol over
the Nordic UART service (`src/phone.c`), which carries time, notifications,
calls, find, battery and more without further services. Done: time,
notifications, calls and find, version and battery. Next: pairing (the chip
keeps the bonds; passkey shown on the watch), a font with more than ASCII,
music control and weather, what the connection costs in current.

Bluetooth again, 22 Nov: Zephyr's host after all. ST's firmware 3.4.0 for
the chip (stack 2.1f, package STSW-BNRG2N) passes data in controller-only
mode, where the factory's 3.2.0 does not. The watch loads it into the chip
itself, over SPI through the chip's updater (`bnrg-update.conf`,
`src/bnrg_update.c`; the README has the steps), and `req-ll-only` is back in
the devicetree. `src/bluetooth.c` is now a user of Zephyr's stack: its
Nordic UART service, the same address, name, advertising and slow pace as
before, and the same interface to the rest of the watch. `src/bluenrg.c`
stays as the updater's way to the chip. What it took besides: a malloc arena
for the host's cryptography (PSA), random numbers from the chip
(`BT_HOST_CRYPTO_PRNG=n`), no flow control from controller to host, as many
ACL buffers as the chip has (7). With it the original phase 5 is open again:
pairing with bonds in settings (it will want the SoC's random number
generator), standard services, mcumgr over Bluetooth.

Seen with it: at every connection the log has "opcode 0x2016 status 0x3a",
the chip being busy when the host asks for the phone's features; nothing
follows from it. And the link was lost once with reason 0x08 some 40 s
after the slow pace took effect, at -39 dBm, as it was under the chip's own
stack (about 25 times in 40 minutes on the same day): so that is the pace,
or the phone, and not the stack. Latency 4 with a 6 s timeout may be too
tight; not looked into yet. The link that stayed up and carried nothing,
seen several times under the chip's stack, has not been seen yet; the probe
for it went with the old code.

Barometer (v0.5.0): the MS5837-02BA is driven by the app (`src/baro.c`,
`src/baro_hw.c`), not by Zephyr's driver, which reads the calibration at
boot, when the sensor here has no supply, and without the pause after reset
or the checksum. On while recording and read once a second at the highest
oversampling; still on the desk it moves by about 3 Pa, a quarter of a
metre. Climb is counted from it in steps of 3 m. The altitude in the file is
the barometer's, moved by the mean difference to the receiver's (the
weather). Not done: what it costs in current, floors.

Accelerometer: the BMI270 has a driver in this tree
(`zephyr/una/drivers/sensor/bmi270`, compatible `una,bmi270`), made of
Zephyr's. Zephyr's has any motion as its only feature, writes the feature
registers with advanced power save on (they read as zeros then, and the
datasheet has the writes done with it off), and takes a single interrupt
line to be INT1, where the watch has INT2 wired. Added: step counter and
detector, activity, wrist-wear wake-up, no motion; the mounting (the stock
firmware's: x is the chip's -y, y its x) and the wrist's angles from the
devicetree; the accelerometer at 50 Hz, power-optimized, only while a
feature is on. Left out: SPI, the FIFO. The app (`src/motion.c`,
`src/motion_hw.c`) starts it after boot (the 8 kB of configuration take a
fifth of a second), has steps and activity counted, asks for the count once
a minute and lights the screen when the wrist is turned (a setting).
Checked on the watch: the chip starts, every feature register reads back as
written, lying flat it reads 0 0 1 g. Not checked: steps against steps
walked, and the wrist, which need the watch worn. Not done: the count is
lost at a restart (the driver resets the chip), nothing shows or stores the
steps but the shell and the log, cadence, what it costs in current (the
datasheet has some tens of microamps).

Magnetometer: the BMM350 too has a driver in this tree
(`zephyr/una/drivers/sensor/bmm350`, compatible `una,bmm350`), made of
Zephyr's. Zephyr's is built on RTIO and selects `I2C_RTIO`, which changes
the I2C controller's driver under every part on every bus; it leaves the
chip measuring from the start, to be stopped only through device power
management, which is not on here; and it gives the field in whole
microtesla, where the level part of the earth's is 16 in Stockholm. This
one is plain I2C, suspended until a rate is set, works the field out as
Bosch's code does, in nanotesla, and turns it into the watch's frame (the
stock firmware's mounting: x is the chip's y, y its x, z its -z). Left out:
I3C, streaming, the data-ready interrupt, the self-test.

The compass (`src/compass.c`, a page in the ring) has the sensor on only
while it is shown: 12.5 readings a second, each the mean of four. The
heading is of twelve o'clock, from magnetic north, with the tilt taken out
by the accelerometer (which the step counter keeps running); while the
wrist is shaken it stays what it was. The watch's own field is found by
turning it every way (top right on the page begins it): the middle of the least
and most each axis has read, kept in settings. Checked on the watch: the
chip starts, reads steadily to within half a microtesla, suspends when the
page is left; lying on the desk it reads 135 53 -46 uT, of which the -46 is
the earth's field going down and most of the rest must be the watch's own.
Not checked: the calibration, and the heading against a compass, which need
the watch turned by hand. Not done: true north, a fit better than least and
most (the stock firmware fits an ellipsoid), the current it costs.

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
