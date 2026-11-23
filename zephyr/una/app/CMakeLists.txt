cmake_minimum_required(VERSION 3.20.0)

set(BOARD una_watch)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(una_zephyr)

target_sources(app PRIVATE
  src/alarm.c
  src/alert.c
  src/baro.c
  src/clock.c
  src/compass.c
  src/compass_page.c
  src/dial.c
  src/draw.c
  src/face.c
  src/fit.c
  src/fonts/font_text_14.c
  src/fonts/font_text_20.c
  src/gnss.c
  src/hr.c
  src/hr_alg.c
  src/main.c
  src/messages.c
  src/motion.c
  src/pairing.c
  src/phone.c
  src/record.c
  src/record_page.c
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
    src/baro_hw.c
    src/compass_hw.c
    src/hr_hw.c
    src/motion_hw.c
    src/power.c
    src/storage.c
    src/usb.c
    src/watchdog.c
  )
  # The radio, or in its place what loads the radio chip's firmware
  # (-DEXTRA_CONF_FILE=bnrg-update.conf).
  if(CONFIG_UNA_BNRG_UPDATE)
    get_filename_component(bnrg_fw ${CONFIG_UNA_BNRG_FW} ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
    if(NOT EXISTS ${bnrg_fw})
      message(FATAL_ERROR "CONFIG_UNA_BNRG_FW: there is no ${bnrg_fw}")
    endif()
    generate_inc_file_for_target(app ${bnrg_fw}
      ${ZEPHYR_BINARY_DIR}/include/generated/bnrg_fw.inc)
    target_sources(app PRIVATE src/bluenrg.c src/bnrg_update.c)
  else()
    target_sources(app PRIVATE src/bluetooth.c)
  endif()
  # PixArt's library on the heart-rate sensor (-DEXTRA_CONF_FILE=hr-ref.conf).
  if(CONFIG_UNA_PIXART_REF)
    get_filename_component(pixart ${CONFIG_UNA_PIXART_DIR} ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
    if(NOT EXISTS ${pixart}/lib/libuna_vendor.a)
      message(FATAL_ERROR "CONFIG_UNA_PIXART_DIR: there is no ${pixart}/lib/libuna_vendor.a")
    endif()
    target_sources(app PRIVATE src/hr_ref.c)
    target_include_directories(app PRIVATE ${pixart}/include/pixart ${pixart}/include/pixart/alg)
    target_link_libraries(app PRIVATE ${pixart}/lib/libuna_vendor.a)
  endif()
else()
  target_sources(app PRIVATE src/sim.c)
endif()
