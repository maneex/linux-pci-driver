#ifndef VELOCITOR_CDEV_SYSFS_H
#define VELOCITOR_CDEV_SYSFS_H

// Linux headers.
#include <linux/sysfs.h>

/// Attribute groups published on /sys/class/velocitor/velocitorN/
extern const struct attribute_group *velocitor_cdev_groups[];

#endif // not VELOCITOR_CDEV_SYSFS_H
