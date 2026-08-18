#ifndef VELOCITOR_REMOTEPROC_H
#define VELOCITOR_REMOTEPROC_H

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "device.h"

#define VEL_VRING_SIZE PAGE_ALIGN(vring_size(VEL_VRING_NUM, VEL_VRING_ALIGN))

int velocitor_remoteproc_initialize(struct pci_dev *pci);

#endif // not VELOCITOR_REMOTEPROC_H
