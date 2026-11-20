cmake_minimum_required(VERSION 3.20.0)

set(BOARD una_watch)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(una_zephyr)

target_sources(app PRIVATE
  src/bluetooth.c
  src/main.c
  src/power.c
  src/screenshot.c
  src/usb.c
  src/watchdog.c
)
