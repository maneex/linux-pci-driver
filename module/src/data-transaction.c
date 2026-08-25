// Linux headers
#include <linux/idr.h>

// Driver headers.
#include "data-transaction.h"
#include "device.h"

void velocitor_data_transaction_reset(
    struct velocitor_dev *device,
    struct velocitor_data_transaction *transaction) {
  int slot = transaction->slot_idx;
  u16 engine = transaction->engine;

  memset(transaction, 0x00, sizeof(struct velocitor_data_transaction));
  transaction->slot_idx = -1;

  if (-1 != slot)
    ida_free(&device->data.queues[engine].slots, slot);
}

int velocitor_data_transaction_init(
    struct velocitor_dev *device, u32 engine,
    struct velocitor_data_transaction **transaction) {
  int index = ida_alloc_max(&device->data.queues[engine].slots,
                            VEL_DATA_INFLIGHT - 1, GFP_ATOMIC);
  if (index < 0)
    return index;

  *transaction = device->data.queues[engine].mem.cpu +
                 index * sizeof(struct velocitor_data_transaction);

  (*transaction)->header.seq =
      cpu_to_le32(atomic_fetch_add(1, &device->data.seq));
  (*transaction)->slot_idx = index;
  (*transaction)->engine = engine;

  return 0;
}
