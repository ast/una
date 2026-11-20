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
 * LVGL. Holding BACK returns to the watch face from anywhere and is not
 * passed on.
 */
struct ui_screen {
	const char *name;
	/* Create the widgets on the active LVGL screen, which is empty. */
	void (*enter)(void);
	/* Bring the widgets up to date. Returns the ms until it is due again. */
	uint32_t (*update)(void);
	void (*key)(enum ui_key key, bool held);
};

/* Screen stack; from enter/update/key only. */
void ui_push(const struct ui_screen *screen);
void ui_pop(void);
void ui_home(void);

/* Any thread: have the current screen updated now. */
void ui_refresh(void);

/* Any thread: have the current screen built again. */
void ui_reload(void);

/* Any thread: put this screen on top, unless it is there, and light up. */
void ui_show(const struct ui_screen *screen);

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
};

void ui_list_create(struct ui_list *list, const char *title, int count);
/* text(i) is the row's text; it is copied. */
void ui_list_show(struct ui_list *list, const char *(*text)(int index));
/* Moves the selection for UP and DOWN; false for the other keys. */
bool ui_list_key(struct ui_list *list, enum ui_key key);

/* A label in one of the three fonts, centred on the screen plus an offset. */
lv_obj_t *ui_label(const lv_font_t *font, uint32_t rgb, int y);

/* "Question?" with SELECT for yes; yes() runs after the screen has closed. */
void ui_confirm(const char *question, void (*yes)(void));

extern const struct ui_screen face_screen;
extern const struct ui_screen face_chooser_screen;
extern const struct ui_screen status_screen;
extern const struct ui_screen menu_screen;
extern const struct ui_screen settings_screen;
extern const struct ui_screen stopwatch_screen;
extern const struct ui_screen timer_screen;
extern const struct ui_screen alarm_screen;

#endif
