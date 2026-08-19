// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers
#include "device.h"
#include "identity.h"

static int velocitor_identity_debugfs_show(struct seq_file *s, void *unused) {
  struct velocitor_dev *device = pci_get_drvdata((struct pci_dev *)s->private);
  seq_printf(s, "version:       %u.%u\n", device->identity.version.major,
             device->identity.version.minor);
  seq_printf(s, "topology:      nodes=%u engines=%u memory=%u\n",
             device->identity.topology.nodes, device->identity.topology.engines,
             device->identity.topology.mem_size);
  seq_printf(s, "capabilities:  ");
  if (0 != (device->identity.caps & VEL_CAP_BF16))
    seq_printf(s, "BF16 ");
  if (0 != (device->identity.caps & VEL_CAP_FP32))
    seq_printf(s, "FP32 ");
  if (0 != (device->identity.caps & VEL_CAP_TRANSPOSE))
    seq_printf(s, "TRANSPOSE");
  seq_printf(s, "\n");
  return 0;
}
DEFINE_SHOW_ATTRIBUTE(velocitor_identity_debugfs);

int velocitor_identity_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  // Read version.
  u32 version = readl(device->bar0 + VEL_REG_VERSION);
  device->identity.version.minor = lower_16_bits(version);
  device->identity.version.major = upper_16_bits(version);

  u32 topology = readl(device->bar0 + VEL_REG_TOPOLOGY);
  device->identity.topology.nodes = upper_16_bits(topology);
  device->identity.topology.engines = lower_16_bits(topology);
  device->identity.topology.mem_size = readl(device->bar0 + VEL_REG_MEM_SIZE);

  device->identity.caps = readl(device->bar0 + VEL_REG_CAPS);

  debugfs_create_file("identity", 0444, device->debugfs, dev,
                      &velocitor_identity_debugfs_fops);

  dev_info(&dev->dev, "bar0: device version %d.%d", version >> 16,
           version & 0xffff);

  return 0;
}
