/*
 * "screenshot": what the panel shows, read back from the display driver and
 * printed as base64, one line of text per row of pixels. scripts/una-z turns
 * it into a PNG.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static int cmd_screenshot(const struct shell *sh, size_t argc, char **argv)
{
	struct display_capabilities caps;
	static uint8_t row[240 * 2];
	static uint8_t text[((sizeof(row) + 2) / 3) * 4 + 1];
	size_t len;

	display_get_capabilities(display, &caps);
	if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565 ||
	    caps.x_resolution * 2U > sizeof(row)) {
		shell_error(sh, "unsupported display");
		return -ENOTSUP;
	}

	const struct display_buffer_descriptor desc = {
		.buf_size = caps.x_resolution * 2U,
		.width = caps.x_resolution,
		.height = 1,
		.pitch = caps.x_resolution,
	};

	shell_print(sh, "SCREENSHOT %u %u RGB565LE", caps.x_resolution, caps.y_resolution);
	for (uint16_t y = 0; y < caps.y_resolution; y++) {
		int ret = display_read(display, 0, y, &desc, row);

		if (ret < 0) {
			shell_error(sh, "read failed: %d", ret);
			return ret;
		}
		base64_encode(text, sizeof(text), &len, row, desc.buf_size);
		shell_print(sh, "%s", text);
	}
	shell_print(sh, "END");
	return 0;
}
SHELL_CMD_REGISTER(screenshot, NULL, "Dump the screen as base64 RGB565", cmd_screenshot);
