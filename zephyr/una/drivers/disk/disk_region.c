/*
 * Sector ranges of a parent disk as disks of their own.
 */

#define DT_DRV_COMPAT una_disk_region

#include <zephyr/device.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>

LOG_MODULE_REGISTER(disk_region, CONFIG_DISK_LOG_LEVEL);

struct region_config {
	const char *parent;
	uint32_t start;
	uint32_t count; /* 0: to the end of the parent */
	bool read_only;
};

struct region_data {
	struct disk_info info;
	uint32_t count;
};

static int region_sector_count(const struct region_config *cfg, uint32_t *count)
{
	uint32_t parent_count;
	int ret;

	ret = disk_access_ioctl(cfg->parent, DISK_IOCTL_GET_SECTOR_COUNT, &parent_count);
	if (ret < 0) {
		return ret;
	}
	if (cfg->start >= parent_count || cfg->count > parent_count - cfg->start) {
		LOG_ERR("region does not fit %s (%u sectors)", cfg->parent, parent_count);
		return -EINVAL;
	}
	*count = cfg->count != 0 ? cfg->count : parent_count - cfg->start;
	return 0;
}

static int region_init(struct disk_info *disk)
{
	const struct region_config *cfg = disk->dev->config;
	struct region_data *data = disk->dev->data;
	int ret;

	ret = disk_access_ioctl(cfg->parent, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret < 0) {
		return ret;
	}
	ret = region_sector_count(cfg, &data->count);
	if (ret < 0) {
		disk_access_ioctl(cfg->parent, DISK_IOCTL_CTRL_DEINIT, NULL);
	}
	return ret;
}

static int region_status(struct disk_info *disk)
{
	const struct region_config *cfg = disk->dev->config;

	return disk_access_status(cfg->parent);
}

static bool in_range(const struct region_data *data, uint32_t start, uint32_t num)
{
	return start < data->count && num <= data->count - start;
}

static int region_read(struct disk_info *disk, uint8_t *buf, uint32_t start, uint32_t num)
{
	const struct region_config *cfg = disk->dev->config;
	struct region_data *data = disk->dev->data;

	if (!in_range(data, start, num)) {
		return -EINVAL;
	}
	return disk_access_read(cfg->parent, buf, cfg->start + start, num);
}

static int region_write(struct disk_info *disk, const uint8_t *buf, uint32_t start, uint32_t num)
{
	const struct region_config *cfg = disk->dev->config;
	struct region_data *data = disk->dev->data;

	if (cfg->read_only) {
		return -EROFS;
	}
	if (!in_range(data, start, num)) {
		return -EINVAL;
	}
	return disk_access_write(cfg->parent, buf, cfg->start + start, num);
}

static int region_ioctl(struct disk_info *disk, uint8_t cmd, void *buf)
{
	const struct region_config *cfg = disk->dev->config;
	struct region_data *data = disk->dev->data;

	switch (cmd) {
	case DISK_IOCTL_CTRL_INIT:
		return region_init(disk);
	case DISK_IOCTL_GET_SECTOR_COUNT:
		*(uint32_t *)buf = data->count;
		return 0;
	default:
		/* Sector size, sync, deinit: the parent's business. */
		return disk_access_ioctl(cfg->parent, cmd, buf);
	}
}

static const struct disk_operations region_ops = {
	.init = region_init,
	.status = region_status,
	.read = region_read,
	.write = region_write,
	.ioctl = region_ioctl,
};

static int region_register(const struct device *dev)
{
	struct region_data *data = dev->data;

	data->info.dev = dev;
	return disk_access_register(&data->info);
}

#define REGION_DEFINE(n)                                                                           \
	static const struct region_config region_config_##n = {                                    \
		.parent = DT_INST_PROP(n, parent_disk),                                            \
		.start = DT_INST_PROP(n, start_sector),                                            \
		.count = DT_INST_PROP_OR(n, sector_count, 0),                                      \
		.read_only = DT_INST_PROP(n, read_only),                                           \
	};                                                                                         \
	static struct region_data region_data_##n = {                                              \
		.info = {.name = DT_INST_PROP(n, disk_name), .ops = &region_ops},                  \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, region_register, NULL, &region_data_##n, &region_config_##n,      \
			      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(REGION_DEFINE)
