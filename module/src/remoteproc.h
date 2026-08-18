#ifndef VELOCITOR_REMOTEPROC_H
#define VELOCITOR_REMOTEPROC_H

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "device.h"

int velocitor_remoteproc_initialize(struct pci_dev *pci);

#endif // not VELOCITOR_REMOTEPROC_H
