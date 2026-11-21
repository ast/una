cmake_minimum_required(VERSION 3.20.0)

set(BOARD una_watch)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(una_zephyr)

target_sources(app PRIVATE
  src/alarm.c
  src/alert.c
  src/clock.c
  src/dial.c
  src/draw.c
  src/face.c
  src/main.c
  src/screenshot.c
  src/settings.c
  src/stopwatch.c
  src/systems.c
  src/timer.c
  src/ui.c
)

# The hardware, or the simulator's stand-ins for it (-b native_sim/native/64).
if(CONFIG_BOARD_UNA_WATCH)
  target_sources(app PRIVATE
    src/alert_hw.c
    src/bluetooth.c
    src/power.c
    src/storage.c
    src/usb.c
    src/watchdog.c
  )
else()
  target_sources(app PRIVATE src/sim.c)
endif()
