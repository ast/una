/*
 * Pairing: the passkey a phone has to be given before it may talk to the
 * watch. The radio says when (src/bluetooth.c); this is the screen.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "pairing.h"
#include "ui.h"

static atomic_t shown;
static unsigned int passkey;
static void (*cancel)(void);

static void pairing_enter(void)
{
	char text[8];

	/* In threes, as phones show it when it is theirs to show. */
	snprintk(text, sizeof(text), "%03u %03u", passkey / 1000 % 1000, passkey % 1000);
	ui_title("PAIR");
	lv_label_set_text(ui_label(&lv_font_montserrat_48, 0xffffff, -10), text);
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, 40), "enter on the phone");
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, 76), "any button: cancel");
}

static uint32_t pairing_update(void)
{
	if (!atomic_get(&shown)) {
		ui_pop();
	}
	return UINT32_MAX;
}

static bool pairing_key(enum ui_key key, bool held)
{
	ARG_UNUSED(key);
	ARG_UNUSED(held);

	ui_pop();
	return true;
}

/* Gone while the phone still waits, by a button or under something else. */
static void pairing_leave(void)
{
	if (atomic_cas(&shown, 1, 0) && cancel != NULL) {
		cancel();
	}
}

static const struct ui_screen pairing_screen = {
	.name = "pairing",
	.enter = pairing_enter,
	.update = pairing_update,
	.key = pairing_key,
	.leave = pairing_leave,
};

void pairing_show(unsigned int key, void (*on_cancel)(void))
{
	passkey = key;
	cancel = on_cancel;
	atomic_set(&shown, 1);
	ui_show(&pairing_screen);
}

void pairing_hide(void)
{
	if (atomic_cas(&shown, 1, 0)) {
		/* The screen takes itself away when it is next updated. */
		ui_refresh();
	}
}
