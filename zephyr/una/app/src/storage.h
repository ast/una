#ifndef UNA_STORAGE_H_
#define UNA_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

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
