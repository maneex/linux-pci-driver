#ifndef VELOCITOR_VRING_H
#define VELOCITOR_VRING_H

// Linux headers
#include <linux/pci.h>
#include <linux/virtio_ring.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver header
#include "dma.h"

#define VEL_VRINGS_COUNT 4

#define VEL_VRING_SIZE PAGE_ALIGN(vring_size(VEL_VRING_NUM, VEL_VRING_ALIGN))

struct velocitor_vring {
  int index;
  int irq;
  int vector;
  int notifyid;
  struct pci_dev *dev;
  struct velocitor_dma_buf mem;
  char *vector_name;
  bool enabled;
};

int velocitor_vrings_initialize(struct pci_dev *dev);
void velocitor_vrings_activate(struct pci_dev *dev);
void velocitor_vrings_invalidate(struct pci_dev *dev);

#endif // not VELOCITOR_VRING_H
