#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

#include <linux/mutex.h>

struct Device {
  void __iomem *bar0;
  void __iomem *bar2;

  struct mutex lock_counters;
};

#endif // not VELOCITOR_DEVICE_H
