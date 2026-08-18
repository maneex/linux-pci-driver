#ifndef VELOCITOR_WINDOW_H
#define VELOCITOR_WINDOW_H

// Linux headers.
#include <linux/pci.h>
#include <linux/types.h>

int velocitor_window_initialize(struct pci_dev *dev);
int velocitor_window_read(struct pci_dev *dev, void *dst, size_t offset,
                          size_t size);

#endif // not VELOCITOR_WINDOW_H
