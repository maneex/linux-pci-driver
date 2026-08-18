// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers
#include "device.h"
#include "identity.h"

static int velocitor_identity_debugfs_show(struct seq_file *s, void *unused) {
  struct velocitor_dev *device = pci_get_drvdata((struct pci_dev *)s->private);
  seq_printf(s, "version              %d.%d\n", device->identity.version.major,
             device->identity.version.minor);
  return 0;
}
DEFINE_SHOW_ATTRIBUTE(velocitor_identity_debugfs);

int velocitor_identity_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  // Read version.
  u32 version = readl(device->bar0 + VEL_REG_VERSION);
  device->identity.version.minor = lower_16_bits(version);
  device->identity.version.major = upper_16_bits(version);

  debugfs_create_file("identity", 0444, device->debugfs, dev,
                      &velocitor_identity_debugfs_fops);

  dev_info(&dev->dev, "bar0: device version %d.%d", version >> 16,
           version & 0xffff);

  return 0;
}
