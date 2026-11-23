#ifndef UNA_PAIRING_H_
#define UNA_PAIRING_H_

/*
 * The screen for pairing with a phone: the six digits the phone asks for.
 * From any thread.
 */

/* Shows it. A button on the watch calls cancel() and takes the screen away. */
void pairing_show(unsigned int passkey, void (*cancel)(void));
/* Takes it away: paired, failed or given up. */
void pairing_hide(void);

#endif
