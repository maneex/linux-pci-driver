// Linux headers.
#include <linux/pci.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>

// Driver headers.
#include "data-transaction.h"
#include "data.h"
#include "device.h"

// https://docs.kernel.org/driver-api/virtio/writing_virtio_drivers.html
// https://www.redhat.com/en/blog/virtqueues-and-virtio-ring-how-data-travels

static void velocitor_data_queue_destroy(void *data) {
  struct velocitor_data_queue *queue = (struct velocitor_data_queue *)data;
  ida_destroy(&queue->slots);
}

void velocitor_data_complete(struct velocitor_dev *device,
                             struct velocitor_data_transaction *transaction,
                             size_t size) {
  // FIXME ! do something.
}

int velocitor_data_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  for (unsigned int i = 0; i < VEL_ENGINES; ++i) {
    ida_init(&device->data.queues[i].slots);

    device->data.queues[i].mem.cpu = dmam_alloc_coherent(
        &dev->dev,
        VEL_DATA_INFLIGHT * sizeof(struct velocitor_data_transaction),
        &device->data.queues[i].mem.dma, GFP_KERNEL);

    if (NULL == device->data.queues[i].mem.cpu)
      return -ENOMEM;

    for (unsigned j = 0; j < VEL_DATA_INFLIGHT; ++j)
      ((struct velocitor_data_transaction *)device->data.queues[i].mem.cpu)[j]
          .slot_idx = -1;

    if ((err = devm_add_action_or_reset(&dev->dev, velocitor_data_queue_destroy,
                                        &device->data.queues[i])))
      return err;
  }

  return 0;
}
