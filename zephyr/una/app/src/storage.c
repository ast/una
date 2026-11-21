/*
 * The eMMC's user volume, the one the stock firmware calls "UNA WATCH"
 * (exFAT), mounted at /USER:.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>

#include <soc.h>

#include <ff.h>

#include "storage.h"

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

static FATFS fat_fs;

static struct fs_mount_t user_mount = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/USER:",
};

static bool mounted;

bool storage_info(struct storage_info *info, bool measure)
{
	static struct storage_info last;
	struct fs_statvfs vfs;

	if (!mounted) {
		return false;
	}
	if (measure && fs_statvfs(user_mount.mnt_point, &vfs) == 0) {
		uint64_t block = vfs.f_frsize;

		last.total_mb = (block * vfs.f_blocks) >> 20;
		last.free_mb = (block * vfs.f_bfree) >> 20;
	}
	*info = last;
	return true;
}

static int storage_init(void)
{
	int ret;

	/*
	 * Hardware flow control: the controller pauses the card's clock when
	 * its FIFO runs empty or full. Zephyr's driver moves data by polling
	 * and offers the option for other STM32 series only; without it every
	 * write here ends in a FIFO underrun. Set after the driver's own init,
	 * which the extra reference also keeps from being undone.
	 */
	ret = disk_access_ioctl("MMC", DISK_IOCTL_CTRL_INIT, NULL);
	if (ret < 0) {
		LOG_ERR("eMMC did not start: %d", ret);
		return 0;
	}
	SDMMC1->CLKCR |= SDMMC_CLKCR_HWFC_EN;

	ret = fs_mount(&user_mount);

	if (ret < 0) {
		LOG_ERR("user volume not mounted: %d", ret);
		return 0; /* the watch works without it */
	}
	mounted = true;
	LOG_INF("user volume mounted");
	return 0;
}
SYS_INIT(storage_init, APPLICATION, 95);
