/*
 * The watch's pulse algorithm (zephyr/una/app/src/hr_alg.c) run over what
 * was recorded of the sensor: see scripts/hr-replay.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hr_alg.h"

#define MAX_N 64

static long numbers(char *text, long *out, int max)
{
	long n = 0;

	for (char *tok = strtok(text, " \n"); tok != NULL && n < max; tok = strtok(NULL, " \n")) {
		out[n++] = strtol(tok, NULL, 10);
	}
	return n;
}

int main(int argc, char **argv)
{
	static char line[16384];
	static long v[4096];
	struct hr_alg_sample samples[MAX_N];
	struct hr_alg_out out = {0};
	int n = 0, worn = 0, verbose = argc > 2;
	/* PixArt's library was given the accelerometer in 8 g to 16 bits, at 50 Hz. */
	int counts = 1;
	long seconds = 0, both = 0, within = 0, ours = 0, theirs = 0;
	double sum_abs = 0;
	FILE *f = fopen(argv[1], "r");

	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	hr_alg_reset();
	while (fgets(line, sizeof(line), f) != NULL) {
		char kind;
		long ms;
		int at;

		if (sscanf(line, "%ld %c%n", &ms, &kind, &at) < 2) {
			continue;
		}
		if (kind == '#' && strstr(line, "accel mg") != NULL) {
			counts = 0;
		} else if (kind == 'P') {
			long c = numbers(line + at, v, 4096);
			int ch = (int)v[1];

			worn = v[0] == 1;
			n = (int)v[2] > MAX_N ? MAX_N : (int)v[2];
			if (c < 3 + n * ch) {
				n = 0;
			}
			for (int i = 0; i < n; i++) {
				samples[i].light = 0;
				for (int k = 0; k < ch; k++) {
					samples[i].light += (uint32_t)v[3 + i * ch + k] / ch;
				}
			}
			if (!worn) {
				hr_alg_reset();
			}
		} else if (kind == 'C') {
			long c = numbers(line + at, v, 4096);
			int ch = n > 0 ? (int)(c / n) : 1;

			for (int i = 0; i < n; i++) {
				samples[i].led = (uint8_t)v[i * ch];
			}
		} else if (kind == 'A') {
			long c = numbers(line + at, v, 4096);
			long have = c > 0 ? v[0] : 0;

			for (int i = 0; i < n; i++) {
				/* The nearest in time of however many there are. */
				long j = have > 0 ? i * have / n : 0;

				for (int axis = 0; axis < 3; axis++) {
					long a = have > 0 ? v[1 + j * 3 + axis] : 0;

					samples[i].accel_mg[axis] = (int16_t)(counts ? a * 8000 / 32768 : a);
				}
			}
			if (n > 0 && worn) {
				hr_alg_feed(samples, n, &out);
			} else {
				memset(&out, 0, sizeof(out));
			}
		} else if (kind == 'H') {
			long c = numbers(line + at, v, 4096);
			long lib = c >= 2 && v[0] == 48 ? v[1] : 0;

			seconds++;
			ours += out.bpm10 != 0;
			theirs += lib != 0;
			if (out.bpm10 != 0 && lib != 0) {
				double d = (out.bpm10 - lib) / 10.0;

				both++;
				sum_abs += d < 0 ? -d : d;
				within += d > -5 && d < 5;
			}
			if (verbose) {
				printf("%ld\t%d.%d\t%d.%d\t%u\t%s\t%ld.%ld\n", ms / 1000, out.bpm10 / 10,
				       out.bpm10 % 10, out.raw_bpm10 / 10, out.raw_bpm10 % 10,
				       out.quality, out.moving ? "moving" : "still", lib / 10, lib % 10);
			}
		}
	}
	printf("%s: %ld s; a pulse from ours in %ld, from the library in %ld; in the %ld with both, "
	       "%.1f apart on average, %ld%% within 5\n",
	       argv[1], seconds, ours, theirs, both, both > 0 ? sum_abs / both : 0.0,
	       both > 0 ? within * 100 / both : 0);
	return 0;
}
