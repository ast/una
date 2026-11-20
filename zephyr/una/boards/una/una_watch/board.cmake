# No SWD without a debug adapter: images are installed through the stock
# bootloader (see scripts/una-pack). The runners are for when one exists.
board_runner_args(openocd "--no-halt")
board_runner_args(jlink "--device=STM32U5A5QJ" "--reset-after-load")
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
