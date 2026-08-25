// Linux headers.
#include <linux/pci.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>

// Driver headers.
#include "data-transaction.h"
#include "data.h"
#include "device.h"
#include "handles.h"

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

static int velocitor_data_transaction_kick(
    struct velocitor_dev *device, struct virtqueue *vq,
    struct velocitor_data_transaction *transaction, size_t op_size) {
  struct scatterlist req;
  struct scatterlist rep;
  struct scatterlist *sgs[2] = {&req, &rep};

  sg_init_one(&req, transaction, sizeof(struct vel_req_hdr) + op_size);
  sg_init_one(&rep, &transaction->response, sizeof(struct vel_resp));

  int err = 0;
  if ((err = virtqueue_add_sgs(vq, sgs, 1, 1, transaction, GFP_ATOMIC))) {
    velocitor_data_transaction_reset(device, transaction);
    return err;
  }

  virtqueue_kick(vq);

  return 0;
}

int velocitor_data_copy(struct velocitor_dev *device, u32 engine, u16 op,
                        struct velocitor_handle *block, u64 offset,
                        dma_addr_t host_addr, u64 len) {
  // Sanity checks.
  if ((NULL == device) || (engine >= VEL_ENGINES) ||
      ((op != VEL_DATA_OP_COPY_H2D) && (op != VEL_DATA_OP_COPY_D2H)))
    return -EINVAL;

  // Check overflow.
  u64 end = 0;
  if (check_add_overflow(offset, len, &end) || (end > block->size))
    return -EINVAL;

  // Retrieve a free slot in our queue.
  int err = 0;
  struct velocitor_data_transaction *transaction = NULL;
  if ((err = velocitor_data_transaction_init(device, engine, &transaction)))
    return err;

  transaction->header.op = cpu_to_le16(op);
  transaction->header.generation = cpu_to_le32(block->generation);
  transaction->op.copy.handle = cpu_to_le32(block->handle);
  transaction->op.copy.offset = cpu_to_le64(offset);
  transaction->op.copy.host.dma_addr = cpu_to_le64(host_addr);
  transaction->op.copy.host.len = cpu_to_le64(len);

  return velocitor_data_transaction_kick(device, device->data.vqs[engine],
                                         transaction,
                                         sizeof(struct vel_copy_hdr));
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
