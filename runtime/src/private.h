#ifndef VELOCITOR_RUNTIME_PRIVATE_H
#define VELOCITOR_RUNTIME_PRIVATE_H

// Velocitor headers.
#include "velocitor/velocitor.h"

/// An open device.
/**
 * \internal
 */
struct velocitor_device {
  /// File descriptor toward char device.
  int fd;

  /// Information about the device.
  struct velocitor_device_info info;
};

struct velocitor_handle {
  struct velocitor_ioctl_alloc alloc;
  const struct velocitor_device *device;
};

// Read device information from sysfs.
/**
 * \internal
 */
int velocitor_sysfs_read_device_attrs(const char *dname,
                                      struct velocitor_device_info *device);

#endif // not VELOCITOR_RUNTIME_PRIVATE_H
