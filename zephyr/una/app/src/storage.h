#ifndef UNA_STORAGE_H_
#define UNA_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

/* Where the user volume is mounted. */
#ifdef CONFIG_BOARD_UNA_WATCH
#define STORAGE_ROOT "/USER:"
#else
/* The simulator mounts a directory of the computer here (-volume). */
#define STORAGE_ROOT "/USER"
#endif

struct storage_info {
	uint32_t total_mb;
	uint32_t free_mb;
};

/*
 * The user volume; false when it is not mounted. Counting what is free takes
 * a while and is done only when `measure` asks for it; otherwise the sizes
 * are those of the last time.
 */
bool storage_info(struct storage_info *info, bool measure);

#endif
