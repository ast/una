#ifndef UNA_STORAGE_H_
#define UNA_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

/* Where the user volume is mounted. */
#ifdef CONFIG_BOARD_UNA_WATCH
#define STORAGE_ROOT "/USER:"
#else
/* The simulator mounts a directory of the computer here (-volume). */
#define STORAGE_ROOT "/USER"
#endif

/*
 * Make a directory unless it is there: the file system writes an error to
 * the log for one that is.
 */
static inline void storage_mkdir(const char *path)
{
	struct fs_dirent entry;

	if (fs_stat(path, &entry) != 0) {
		(void)fs_mkdir(path);
	}
}

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
