#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>

struct Device {
  void __iomem *bar0;
  void __iomem *bar2;

  struct dentry *debugfs;

  struct mutex lock_counters;

  size_t last_known_winbase;
  struct mutex lock_winbase;
};

#endif // not VELOCITOR_DEVICE_H
