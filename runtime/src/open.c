// C headers.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Velocitor headers.
#include "private.h"
#include "velocitor/velocitor.h"

int velocitor_open(const struct velocitor_device_info *info,
                   struct velocitor_device **device) {
  if ((NULL == device) || (NULL == info))
    return -EINVAL;

  *device = NULL;

  char path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "/dev/%s", info->devname);
  if ((n < 0) || ((size_t)n >= sizeof(path)))
    return -ENAMETOOLONG;

  struct velocitor_device *h = calloc(1, sizeof(*h));
  if (NULL == h)
    return -ENOMEM;

  h->fd = open(path, O_RDWR | O_CLOEXEC);
  if (h->fd < 0) {
    int err = -errno;
    free(h);
    return err;
  }

  memcpy(&h->info, info, sizeof(h->info));

  *device = h;
  return 0;
}

int velocitor_close(struct velocitor_device *device) {
  if (NULL == device)
    return 0;

  int err = 0;
  if ((device->fd >= 0) && (close(device->fd) < 0))
    err = -errno;

  free(device);
  return err;
}

const struct velocitor_device_info *
velocitor_info(const struct velocitor_device *device) {
  return (NULL == device) ? NULL : &device->info;
}
