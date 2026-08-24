#ifndef VELOCITOR_COUNTERS_H
#define VELOCITOR_COUNTERS_H

// Linux headers.
#include <linux/pci.h>

// Driver headers.
#include "device.h"
#include "velocitor.h"

void velocitor_counters_reset(struct velocitor_dev *device);
void velocitor_counters_read(struct velocitor_dev *device,
                             struct counters *counters);
int velocitor_counters_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_COUNTERS_H
