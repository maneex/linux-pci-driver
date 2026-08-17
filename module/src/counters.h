#ifndef VELOCITOR_COUNTERS_H
#define VELOCITOR_COUNTERS_H

// Linux headers.
#include <linux/pci.h>

// Driver headers.
#include <velocitor.h>

void velocitor_reset_counters(struct pci_dev *dev);
void velocitor_read_counters(struct pci_dev *dev, struct counters *counters);

#endif // not VELOCITOR_COUNTERS_H
