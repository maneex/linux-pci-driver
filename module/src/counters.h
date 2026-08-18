#ifndef VELOCITOR_COUNTERS_H
#define VELOCITOR_COUNTERS_H

// Linux headers.
#include <linux/pci.h>

// Driver headers.
#include <velocitor.h>

void velocitor_counters_reset(struct pci_dev *dev);
void velocitor_counters_read(struct pci_dev *dev, struct counters *counters);
int velocitor_counters_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_COUNTERS_H
