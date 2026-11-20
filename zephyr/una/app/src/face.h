#ifndef UNA_FACE_H_
#define UNA_FACE_H_

#include <stdbool.h>

/* The chosen watch face's name. src/ui.h has the screen that chooses. */
const char *face_name(void);

/* Analogue dials white, or black. Stored. */
bool face_light(void);
void face_set_light(bool on);

#endif
