#ifndef VELOCITOR_IDENTITY_H
#define VELOCITOR_IDENTITY_H

// Linux headers.
#include <linux/pci.h>

int velocitor_identity_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_IDENTITY_H
