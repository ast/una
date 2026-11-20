#ifndef UNA_ALERT_H_
#define UNA_ALERT_H_

#include <stdbool.h>

/*
 * Beep and vibrate for up to `seconds`, and show `title` until a button is
 * pressed, which also ends the noise. From any thread.
 */
void alert_ring(const char *title, int seconds);
void alert_stop(void);

/* One short beep and buzz. */
void alert_blip(void);

/* What an alert may use. Stored. */
bool alert_sound(void);
void alert_set_sound(bool on);
bool alert_vibration(void);
void alert_set_vibration(bool on);

/* The hardware: src/alert_hw.c on the watch, src/sim.c in the simulator. */
void buzzer_set(bool on);
void motor_set(bool on);

#endif
