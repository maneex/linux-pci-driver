#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

struct Device {
  void __iomem *bar0;
  void __iomem *bar2;
  void __iomem *bar4;
};

enum InitialisationState {
  INIT_STATE_PROBED,
  INIT_STATE_ENABLED,
  INIT_STATE_IOMAP,
  INIT_STATE_COMPLETE
};

#define BAR0_MAGIC_VALUE 0x4F4C4556
#define BAR0_MAGIC_OFFSET 0x000
#define BAR0_SCRATCH_OFFSET 0x00C
#endif // not VELOCITOR_DEVICE_H
