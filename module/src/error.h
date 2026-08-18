#ifndef VELOCITOR_ERROR_H
#define VELOCITOR_ERROR_H

// Driver headers.
#include "device.h"

struct velocitor_error {
  u32 code;
  u64 info;
  u32 notifyid;
  u32 handle;
  u32 generation;
  u32 dropped;
};

void velocitor_error_read(struct velocitor_dev *device,
                          struct velocitor_error *err);

#endif // not VELOCITOR_ERROR_H
