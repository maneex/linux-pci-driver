

// Linux headers.
#include <linux/pci.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver error.
#include "error.h"

void velocitor_error_read(struct velocitor_dev *device,
                          struct velocitor_error *err) {
  memset(err, 0x00, sizeof(struct velocitor_error));

  err->code = readl(device->bar0 + VEL_REG_ERR_CODE);
  err->info = ((u64)readl(device->bar0 + VEL_REG_ERR_INFO_LO)) |
              ((u64)readl(device->bar0 + VEL_REG_ERR_INFO_HI)) << 32;
  err->notifyid = readl(device->bar0 + VEL_REG_ERR_NOTIFYID);
  err->handle = readl(device->bar0 + VEL_REG_ERR_HANDLE);
  err->generation = readl(device->bar0 + VEL_REG_ERR_GENERATION);
  err->dropped = readl(device->bar0 + VEL_REG_ERR_DROPPED);
}
