/*
 * USB device: two serial ports. Interface 0 is cdc_acm_uart0 (shell and log),
 * interface 2 is cdc_acm_uart1 (mcumgr).
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

/* The Zephyr project's test IDs: fine on a desk, not for anything shipped. */
USBD_DEVICE_DEFINE(una_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2fe3, 0x0004);

USBD_DESC_LANG_DEFINE(una_lang);
USBD_DESC_MANUFACTURER_DEFINE(una_mfr, "UNA");
USBD_DESC_PRODUCT_DEFINE(una_product, "UNA Watch Zephyr");
USBD_DESC_SERIAL_NUMBER_DEFINE(una_sn);

USBD_DESC_CONFIG_DEFINE(una_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(una_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(una_fs_config, 0, 125, &una_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(una_hs_config, 0, 125, &una_hs_cfg_desc);

static bool enabled;

/* What usbd_shutdown() removes and every start has to put back. */
static int add_functions(enum usbd_speed speed)
{
	int err = usbd_register_all_classes(&una_usbd, speed, 1, NULL);

	if (err == 0) {
		/* Interface association descriptors are in use. */
		err = usbd_device_set_code_triple(&una_usbd, speed, USB_BCC_MISCELLANEOUS, 0x02,
						  0x01);
	}
	return err;
}

static int usb_start(void)
{
	const bool hs = USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&una_usbd) == USBD_SPEED_HS;
	int err;

	err = usbd_add_descriptor(&una_usbd, &una_lang);
	if (err == 0) {
		err = usbd_add_descriptor(&una_usbd, &una_mfr);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&una_usbd, &una_product);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&una_usbd, &una_sn);
	}
	if (err == 0 && hs) {
		err = add_functions(USBD_SPEED_HS);
	}
	if (err == 0) {
		err = add_functions(USBD_SPEED_FS);
	}
	if (err == 0) {
		err = usbd_init(&una_usbd);
	}
	if (err == 0) {
		err = usbd_enable(&una_usbd);
	}
	return err;
}

static int usb_stop(void)
{
	int err = usbd_disable(&una_usbd);

	/* Shutdown, not only disable: that is what powers the PHY down. */
	if (err == 0) {
		err = usbd_shutdown(&una_usbd);
	}
	return err;
}

/*
 * While enabled, the USB controller keeps the SoC out of Stop mode, so it is
 * only on while there is USB power. Called from the power thread.
 */
void usb_set_enabled(bool on)
{
	static K_MUTEX_DEFINE(lock);
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	if (on != enabled) {
		err = on ? usb_start() : usb_stop();
		if (err != 0) {
			LOG_ERR("USB %s failed: %d", on ? "start" : "stop", err);
		} else {
			enabled = on;
		}
	}
	k_mutex_unlock(&lock);
}

static int usb_init(void)
{
	int err = 0;

	/* Configurations survive a shutdown and are added once. */
	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&una_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&una_usbd, USBD_SPEED_HS, &una_hs_config);
	}
	if (err == 0) {
		err = usbd_add_configuration(&una_usbd, USBD_SPEED_FS, &una_fs_config);
	}
	if (err != 0) {
		LOG_ERR("USB did not start: %d", err);
		return err;
	}
	/* On at boot whatever the power source: a console for the first seconds. */
	usb_set_enabled(true);
	return 0;
}
SYS_INIT(usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
