#ifndef VELOCITOR_HANDLES_H
#define VELOCITOR_HANDLES_H

// Linux headers.
#include <linux/kref.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/xarray.h>

// Driver headers.
#include "ctrl.h"

/// One live device allocation, host side.
struct velocitor_handle {
  // Device handle
  u32 handle;

  // Generation value when allocated.
  u32 generation;

  // Allocated size.
  u64 size;

  // Node.
  u32 node;

  // Offset of the buffer in the device memory.
  u64 dev_offset;

  // Ref counter.
  struct kref refcount;

  // Creation timestamp
  unsigned long created;
};

struct velocitor_handles {
  struct xarray entries;
  u32 generation;
};

// Release a handle.
void velocitor_handles_release(struct velocitor_handle *entry);

// Update the table after a generation update.
void velocitor_handles_regenerate(struct pci_dev *dev);

// Insert a handle into the table.
int velocitor_handles_insert(struct pci_dev *dev, u64 size,
                             const struct velocitor_ctrl_alloc_resp *resp,
                             struct velocitor_handle **dstentry);

// Erase a handle from the table.
int velocitor_handles_erase(struct pci_dev *dev,
                            struct velocitor_handle *handle);

// Module initialization
int velocitor_handles_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_HANDLES_H
