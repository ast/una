#ifndef UNA_UI_H_
#define UNA_UI_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

/* The four buttons, named as the stock firmware names them. */
enum ui_key {
	UI_KEY_UP,     /* SW1, top left */
	UI_KEY_SELECT, /* SW2, top right */
	UI_KEY_DOWN,   /* SW3, bottom left */
	UI_KEY_BACK,   /* SW4, bottom right */
};

/*
 * A screen. All three run in the main thread, the only one that may call
 * LVGL.
 *
 * The buttons mean the same everywhere. UP and DOWN go to what is beside
 * this: the next page in the ring, row in a list, value of a field. SELECT
 * does what the page does, or goes in; held, it goes in to change what the
 * page is set to. BACK goes out one level and, from a page in the ring, to
 * the watch face. Holding BACK returns to the face from anywhere and is not
 * passed on.
 */
struct ui_screen {
	const char *name;
	/* Create the widgets on the active LVGL screen, which is empty. */
	void (*enter)(void);
	/* Bring the widgets up to date. Returns the ms until it is due again. */
	uint32_t (*update)(void);
	/*
	 * False for a key the screen has no use for, which then does what it
	 * does everywhere: UP and DOWN move in the ring, BACK goes out.
	 */
	bool (*key)(enum ui_key key, bool held);
	/* Optional: the screen is no longer the one shown, however that came about. */
	void (*leave)(void);
};

/*
 * The ring's pages lie under a stack of the screens gone into from them.
 * From enter/update/key only.
 */
void ui_push(const struct ui_screen *screen);
void ui_pop(void);
void ui_home(void);

/* Whether a button is down right now: for what must be held for long. */
bool ui_key_is_down(enum ui_key key);

/* Any thread: have the current screen updated now. */
void ui_refresh(void);

/* Any thread: have the current screen built again. */
void ui_reload(void);

/* Any thread: put this screen on top, unless it is there, and light up. */
void ui_show(const struct ui_screen *screen);

/* Any thread: light the screen as a button does. */
void ui_light(void);

/* Does not return. */
void ui_run(void);

/* Seconds the backlight stays on after a button; 0 keeps it off. Stored. */
int ui_backlight_s(void);
void ui_set_backlight_s(int seconds);

/* A title and up to UI_LIST_ROWS visible rows, one of them selected. */
#define UI_LIST_ROWS 5

struct ui_list {
	lv_obj_t *rows[UI_LIST_ROWS];
	int count;
	int selected;
	bool quiet; /* shown without the selection */
};

void ui_list_create(struct ui_list *list, const char *title, int count);
/* text(i) is the row's text; it is copied. */
void ui_list_show(struct ui_list *list, const char *(*text)(int index));
/* Moves the selection for UP and DOWN; false for the other keys. */
bool ui_list_key(struct ui_list *list, enum ui_key key);

/* A label in one of the three fonts, centred on the screen plus an offset. */
lv_obj_t *ui_label(const lv_font_t *font, uint32_t rgb, int y);

/*
 * For text from outside the watch: Montserrat like the rest, with Latin-1
 * and some punctuation besides ASCII (scripts/una-fonts makes them).
 */
LV_FONT_DECLARE(font_text_14);
LV_FONT_DECLARE(font_text_20);
/* Puts a question mark for every character of UTF-8 text they do not have. */
void ui_readable(char *text);

/* The small title at the top of a page. */
void ui_title(const char *title);

extern const struct ui_screen face_screen;
extern const struct ui_screen face_chooser_screen;
extern const struct ui_screen record_screen;
extern const struct ui_screen compass_screen;
extern const struct ui_screen hr_screen;
extern const struct ui_screen messages_screen;
extern const struct ui_screen message_screen;
extern const struct ui_screen settings_screen;
extern const struct ui_screen systems_screen;
extern const struct ui_screen stopwatch_screen;
extern const struct ui_screen timer_screen;
extern const struct ui_screen alarm_screen;

#endif
