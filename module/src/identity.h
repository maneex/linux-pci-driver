#ifndef VELOCITOR_IDENTITY_H
#define VELOCITOR_IDENTITY_H

// Linux headers.
#include <linux/pci.h>

struct velocitor_identity {
  struct {
    u16 minor;
    u16 major;
  } version;

  struct {
    u16 nodes;
    u16 engines;
    u32 mem_size;
  } topology;

  u32 caps;
};

int velocitor_identity_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_IDENTITY_H
