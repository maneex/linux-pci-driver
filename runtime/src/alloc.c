// C headers.
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>

// Project headers
#include "private.h"

int velocitor_alloc(const struct velocitor_device *device, uint64_t size,
                    uint32_t node, uint32_t type,
                    struct velocitor_handle **handle) {
  if ((NULL == device) || (-1 == device->fd) || (NULL == handle))
    return -EINVAL;

  *handle = calloc(1, sizeof(**handle));
  if (NULL == *handle)
    return -ENOMEM;

  (*handle)->device = device;
  (*handle)->alloc.size = size;
  (*handle)->alloc.node = node;
  (*handle)->alloc.dtype = type;

  if (ioctl(device->fd, VEL_IOC_ALLOC_DEV, &(*handle)->alloc) < 0) {
    int err = -errno;
    free(*handle);
    *handle = NULL;
    return err;
  }

  return 0;
}

int velocitor_free(struct velocitor_handle *handle) {
  if (NULL == handle)
    return -EINVAL;

  int err = 0;
  if (ioctl(handle->device->fd, VEL_IOC_FREE_DEV, &handle->alloc.handle) < 0)
    err = -errno;

  free(handle);
  return err;
}
