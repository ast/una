/*
 * USB device: two serial ports. Interface 0 is cdc_acm_uart0 (shell and log),
 * interface 2 is cdc_acm_uart1 (mcumgr).
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
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

static int add_speed(enum usbd_speed speed, struct usbd_config_node *config)
{
	int err = usbd_add_configuration(&una_usbd, speed, config);

	if (err == 0) {
		err = usbd_register_all_classes(&una_usbd, speed, 1, NULL);
	}
	if (err == 0) {
		/* Interface association descriptors are in use. */
		err = usbd_device_set_code_triple(&una_usbd, speed, USB_BCC_MISCELLANEOUS, 0x02,
						  0x01);
	}
	return err;
}

static int usb_start(void)
{
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
	if (err == 0 && USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&una_usbd) == USBD_SPEED_HS) {
		err = add_speed(USBD_SPEED_HS, &una_hs_config);
	}
	if (err == 0) {
		err = add_speed(USBD_SPEED_FS, &una_fs_config);
	}
	if (err == 0) {
		err = usbd_init(&una_usbd);
	}
	if (err == 0) {
		err = usbd_enable(&una_usbd);
	}
	if (err != 0) {
		LOG_ERR("USB did not start: %d", err);
	}
	return err;
}
SYS_INIT(usb_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
