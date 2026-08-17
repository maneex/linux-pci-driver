#ifndef VELOCITOR_DEBUGFS_H
#define VELOCITOR_DEBUGFS_H

// Linux headers.
#include <linux/pci.h>

// Driver headers.
#include "device.h"

int velocitor_debugfs_initialize(struct pci_dev *dev, struct dentry *root);

#endif // not VELOCITOR_DEBUGFS_H
