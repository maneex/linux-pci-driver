#ifndef VELOCITOR_DATA_H
#define VELOCITOR_DATA_H

// Linux headers.
#include <linux/build_bug.h>
#include <linux/types.h>
#include <linux/virtio.h>

// Velocitor headers.
#include <velocitor_hw.h>
#include <velocitor_wire.h>

// Driver headers.
#include "data-transaction.h"
#include "dma.h"

struct velocitor_dev;

struct velocitor_data_queue {
  struct velocitor_dma_buf mem;
  struct ida slots;
};

struct velocitor_data {
  struct virtqueue *vqs[VEL_ENGINES];

  struct velocitor_data_queue queues[VEL_ENGINES];

  atomic_t seq;
};

void velocitor_data_complete(struct velocitor_dev *device,
                             struct velocitor_data_transaction *transaction,
                             size_t size);

int velocitor_data_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_DATA_H
