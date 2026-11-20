/*
 * Zephyr on the UNA Watch: the UI in the main thread, a shell on the USB
 * serial port and mcumgr for loading the next build.
 */

#include "ui.h"

int main(void)
{
	ui_run();
	return 0;
}
