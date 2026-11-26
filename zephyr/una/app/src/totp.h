#ifndef UNA_TOTP_H_
#define UNA_TOTP_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * The codes an authenticator app shows (RFC 6238 as Google Authenticator
 * makes them: HMAC-SHA1, six digits, a new one every thirty seconds). The
 * secrets come in through the shell ("totp add") and are kept in the
 * settings; none is in the image.
 */
#define TOTP_STEP_S   30
#define TOTP_NAME_MAX 12

/* How many there are; they are numbered from 0 in the order they were added. */
int totp_count(void);

/* The name of one, and its code at a time. False if there is no such one. */
bool totp_get(int index, time_t utc, char name[TOTP_NAME_MAX + 1], uint32_t *code);

#endif
