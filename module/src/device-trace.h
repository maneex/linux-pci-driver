#ifndef VELOCITOR_DEVICE_TRACE_H
#define VELOCITOR_DEVICE_TRACE_H

// Linux headers.
#include <linux/mutex.h>
#include <linux/pci.h>

struct velocitor_dtrace {
  struct mutex lock;

  // Where the firmware put its ring, from the header it published (spec 6.6).
  // Only meaningful once a firmware has been verified, so it is read in
  // ops->start() and not at probe: before that, device memory still holds the
  // reset pattern and this would be an offset into nothing.
  u32 da;
};

int velocitor_dtrace_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_DEVICE_TRACE_H
