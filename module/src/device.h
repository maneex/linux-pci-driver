#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

struct Device {
  void __iomem *bar0;
  void __iomem *bar2;
};

enum InitialisationState {
  INIT_STATE_PROBED,
  INIT_STATE_ENABLED,
  INIT_STATE_IOMAP,
  INIT_STATE_DMA,
  INIT_STATE_IRQ,
  INIT_STATE_COMPLETE
};

#endif // not VELOCITOR_DEVICE_H
