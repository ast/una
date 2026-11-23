/*
 * The pulse from the light.
 *
 * The last 12.8 s, 256 samples at 20 Hz, are taken apart into frequencies
 * once a second, and the strongest between 40 and 210 a minute is the pulse:
 * blood fills the skin with each beat and takes some of the green away.
 *
 * What stands in the way, and what is done about each:
 *
 * - The light's level is a hundred to a thousand times its pulse, and drifts.
 *   Not the samples but their differences are taken apart, and the tilt that
 *   gives the spectrum is taken out of it again.
 * - The sensor changes its LED when it likes, which is a step in the light
 *   in spite of the scaling. The difference across a change is left out.
 * - A wrist that moves shakes the sensor on the skin, at the pace of the
 *   steps and of the arm, as strongly as the pulse or more. The
 *   accelerometer shows that pace, and what the light has at the same
 *   frequencies is turned down by as much as the movement has there.
 * - Between two seconds the pulse does not go far. A peak near the last
 *   answer counts for more than one far off, and an answer is only given
 *   when the peak stands clear, some seconds running.
 *
 * All in whole numbers: the watch is built without the FPU or a sine.
 */

#include <string.h>

#include "hr_alg.h"

#define N         256
#define LOG2_N    8
#define MIN_FILL  160 /* 8 s before the first answer */
/* A bin is 20 Hz / 256, or 4.69 a minute. */
#define BIN_LO    9   /* 42 a minute */
#define BIN_HI    45  /* 211 */
#define BPM10_PER_BIN_Q8 ((HR_ALG_HZ * 600 * 256) / N)

/* Peak and its neighbours against all of the band, percent, to be believed. */
#define QUALITY_MIN   25
/* Seconds running that it must be. */
#define GOOD_TO_LOCK  3
/* Seconds without, after which the answer is given up. */
#define BAD_TO_LOSE   8
/* How far the pulse may be from the last, in bins, and still be favoured. */
#define NEAR_BINS     3
/*
 * From one sample to the next the three axes change by more than this, mean
 * square in mg, and the wrist counts as moving.
 */
#define MOVING_MG2    (30 * 30)
/* And its strongest frequency stands so many times above their average. */
#define PACED         6

/* sin(2 pi i / 256) in Q15, the first quarter. */
static const int16_t quarter[N / 4 + 1] = {
	0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,
	8739,  9512,  10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151,
	16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170,
	23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510,
	28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785,
	31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
};

static int32_t sine(unsigned int i)
{
	i &= N - 1;
	if (i <= N / 4) {
		return quarter[i];
	}
	if (i <= N / 2) {
		return quarter[N / 2 - i];
	}
	return -sine(i - N / 2);
}

static int32_t cosine(unsigned int i)
{
	return sine(i + N / 4);
}

/* In place; what comes in must be below 2^15 for the sums to fit. */
static void fft(int32_t *re, int32_t *im)
{
	for (unsigned int i = 1, j = 0; i < N; i++) {
		unsigned int bit = N >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			int32_t t = re[i];

			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (unsigned int len = 2; len <= N; len <<= 1) {
		unsigned int step = N / len;

		for (unsigned int i = 0; i < N; i += len) {
			for (unsigned int k = 0; k < len / 2; k++) {
				int32_t wr = cosine(k * step), wi = -sine(k * step);
				int32_t *ar = &re[i + k], *ai = &im[i + k];
				int32_t *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
				int32_t tr = (int32_t)(((int64_t)*br * wr - (int64_t)*bi * wi) >> 15);
				int32_t ti = (int32_t)(((int64_t)*br * wi + (int64_t)*bi * wr) >> 15);

				*br = *ar - tr;
				*bi = *ai - ti;
				*ar += tr;
				*ai += ti;
			}
		}
	}
}

static struct hr_alg_sample ring[N];
static unsigned int head; /* where the next goes */
static unsigned int fill;
static int32_t re[N], im[N];
static uint64_t light_power[BIN_HI + 2];
static uint64_t accel_power[BIN_HI + 2];

static int lock_bin_q8; /* the answer held, in bins times 256; 0 for none */
static unsigned int good, bad;
static bool locked;

void hr_alg_reset(void)
{
	head = 0;
	fill = 0;
	lock_bin_q8 = 0;
	good = 0;
	bad = 0;
	locked = false;
}

static const struct hr_alg_sample *at(unsigned int i)
{
	return &ring[(head + N - fill + i) & (N - 1)];
}

/*
 * The spectrum of what is in re[0..fill): scaled to 14 bits, under a window
 * that falls to nothing at both ends, the rest of the 256 left at zero.
 */
static void spectrum(uint64_t *power, bool tilted)
{
	int32_t peak = 1;
	int shift = 0;

	for (unsigned int i = 0; i < fill; i++) {
		int32_t a = re[i] < 0 ? -re[i] : re[i];

		peak = a > peak ? a : peak;
	}
	while ((peak >> shift) >= (1 << 14)) {
		shift++;
	}
	for (unsigned int i = 0; i < N; i++) {
		if (i < fill) {
			/* Hann: (1 - cos) / 2, over the part that is filled. */
			int32_t window = (32767 - cosine(i * N / fill)) / 2;

			re[i] = (int32_t)(((int64_t)(re[i] >> shift) * window) >> 15);
		} else {
			re[i] = 0;
		}
		im[i] = 0;
	}
	fft(re, im);
	for (unsigned int k = 0; k <= BIN_HI + 1; k++) {
		power[k] = (uint64_t)((int64_t)re[k] * re[k] + (int64_t)im[k] * im[k]);
		if (tilted && k > 0) {
			/* A difference grows with the frequency; its power by the square. */
			power[k] = power[k] * 64 / (k * k);
		}
	}
}

static bool analyse(struct hr_alg_out *out)
{
	uint64_t accel_peak = 0, accel_sum = 0, total = 0, best_score = 0, near;
	int64_t accel_ms = 0;
	unsigned int best = 0;
	int delta_q8 = 0;
	bool moving;

	/* The light: differences, none across a change of the LED. */
	re[0] = 0;
	for (unsigned int i = 1; i < fill; i++) {
		const struct hr_alg_sample *a = at(i - 1), *b = at(i);

		re[i] = a->led == b->led ? (int32_t)(b->light - a->light) : 0;
	}
	/* A knock on the watch is a spike many times the pulse: held to size. */
	{
		int64_t sum = 0;
		int32_t limit;

		for (unsigned int i = 0; i < fill; i++) {
			sum += re[i] < 0 ? -re[i] : re[i];
		}
		limit = (int32_t)(sum / fill) * 4 + 1;
		for (unsigned int i = 0; i < fill; i++) {
			re[i] = re[i] > limit ? limit : re[i] < -limit ? -limit : re[i];
		}
	}
	spectrum(light_power, true);

	/*
	 * The movement: differences here too, so that a wrist turned over, which
	 * is the pull of the earth going from one axis to another, is none.
	 */
	memset(accel_power, 0, sizeof(accel_power));
	for (int axis = 0; axis < 3; axis++) {
		uint64_t one[BIN_HI + 2];

		re[0] = 0;
		for (unsigned int i = 1; i < fill; i++) {
			re[i] = at(i)->accel_mg[axis] - at(i - 1)->accel_mg[axis];
			accel_ms += (int64_t)re[i] * re[i];
		}
		spectrum(one, true);
		for (unsigned int k = 0; k <= BIN_HI + 1; k++) {
			/* Each was scaled by itself: only the shape is kept. */
			accel_power[k] += one[k] >> 8;
		}
	}
	for (unsigned int k = BIN_LO - 1; k <= BIN_HI + 1; k++) {
		accel_peak = accel_power[k] > accel_peak ? accel_power[k] : accel_peak;
		accel_sum += accel_power[k];
	}
	/*
	 * Only a movement with a pace to it is taken out of the light: steps,
	 * an arm swung. A hand that types or reaches has none, and all it
	 * would do is turn down whatever the pulse happens to share with it.
	 */
	moving = accel_ms / fill > MOVING_MG2 &&
		 accel_peak > accel_sum / (BIN_HI - BIN_LO + 3) * PACED;
	out->moving = moving;
	if (moving) {
		for (unsigned int k = BIN_LO - 1; k <= BIN_HI + 1; k++) {
			/* Down by as much as the movement has here, to a tenth at its peak. */
			uint64_t share = accel_power[k] * 230 / accel_peak;

			light_power[k] = light_power[k] / 256 * (256 - share);
		}
	}

	for (unsigned int k = BIN_LO; k <= BIN_HI; k++) {
		uint64_t score = light_power[k];
		int away;

		total += light_power[k];
		if (light_power[k] < light_power[k - 1] || light_power[k] < light_power[k + 1]) {
			continue;
		}
		if (lock_bin_q8 != 0) {
			away = (int)k - (lock_bin_q8 + 128) / 256;
			away = away < 0 ? -away : away;
			/* Far from the last answer it must be four times as strong. */
			score = away <= NEAR_BINS ? score : score / 4;
		}
		if (score > best_score) {
			best_score = score;
			best = k;
		}
	}
	if (best == 0 || total == 0) {
		out->quality = 0;
		return false;
	}
	near = light_power[best] + light_power[best - 1] + light_power[best + 1];
	/* Its neighbours outside the band were not counted into the whole. */
	total += best == BIN_LO ? light_power[best - 1] : 0;
	total += best == BIN_HI ? light_power[best + 1] : 0;
	out->quality = (uint8_t)(near * 100 / total);

	/* Between the bins, by the parabola through the three (of their roots, roughly). */
	{
		int64_t l = (int64_t)(light_power[best - 1] >> 8);
		int64_t c = (int64_t)(light_power[best] >> 8);
		int64_t r = (int64_t)(light_power[best + 1] >> 8);
		int64_t den = 2 * (2 * c - l - r);

		if (den > 0) {
			delta_q8 = (int)((r - l) * 256 / den);
			delta_q8 = delta_q8 > 128 ? 128 : delta_q8 < -128 ? -128 : delta_q8;
		}
	}
	out->raw_bpm10 = (uint16_t)(((int64_t)(best * 256 + delta_q8) * BPM10_PER_BIN_Q8) >> 16);

	if (out->quality < QUALITY_MIN) {
		return false;
	}
	if (lock_bin_q8 == 0) {
		lock_bin_q8 = (int)best * 256 + delta_q8;
	} else {
		/* A quarter of the way to the new one each second. */
		lock_bin_q8 += ((int)best * 256 + delta_q8 - lock_bin_q8) / 4;
	}
	return true;
}

void hr_alg_feed(const struct hr_alg_sample *samples, unsigned int count, struct hr_alg_out *out)
{
	memset(out, 0, sizeof(*out));
	for (unsigned int i = 0; i < count; i++) {
		ring[head] = samples[i];
		head = (head + 1) & (N - 1);
		fill = fill < N ? fill + 1 : N;
	}
	if (fill < MIN_FILL) {
		return;
	}
	if (analyse(out)) {
		good++;
		bad = 0;
		locked = locked || good >= GOOD_TO_LOCK;
	} else {
		/* Held through a bad second or two, once it has been good. */
		bad++;
		good = 0;
		if (bad >= BAD_TO_LOSE) {
			locked = false;
			lock_bin_q8 = 0;
		}
	}
	if (locked) {
		out->bpm10 = (uint16_t)(((int64_t)lock_bin_q8 * BPM10_PER_BIN_Q8) >> 16);
	}
}
