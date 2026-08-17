#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

struct Device {
  void __iomem *bar0;
  void __iomem *bar2;
};

#endif // not VELOCITOR_DEVICE_H
