// Linux headers.
#include <linux/uaccess.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "counters.h"
#include "debugfs.h"
#include "dma.h"
#include "window.h"

/**
 * DebugFS / Inject error
 */
static int velocitor_debugfs_inject_error(void *dev, u64 cmd) {
  if (cmd > 255)
    return -EINVAL;

  struct velocitor_dev *device = pci_get_drvdata(dev);
  writel(cmd, device->bar0 + VEL_REG_ERR_INJECT);
  return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(velocitor_debugfs_inject_error_fops, NULL,
                         velocitor_debugfs_inject_error, "%llu\n");

static void velocitor_debugfs_release(void *root) {
  debugfs_remove_recursive(root);
}

/**
 * DebugFS / Initialize.
 */

int velocitor_debugfs_initialize(struct pci_dev *dev, struct dentry *root) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  device->debugfs = debugfs_create_dir(pci_name(dev), root);
  if ((err = devm_add_action_or_reset(&dev->dev, velocitor_debugfs_release,
                                      device->debugfs)))
    return err;

  debugfs_create_file_unsafe("inject_error", 0200, device->debugfs, dev,
                             &velocitor_debugfs_inject_error_fops);

  return 0;
}
