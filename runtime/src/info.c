// C headers.
#include <errno.h>
#include <sys/ioctl.h>

// Project headers
#include "private.h"

int velocitor_state(const struct velocitor_device *device,
                    struct velocitor_state *state) {
  if ((NULL == device) || (-1 == device->fd) || (NULL == state))
    return -EINVAL;

  struct velocitor_ioctl_info info = {};
  if (ioctl(device->fd, VEL_IOC_INFO, &info) < 0)
    return -errno;

  state->state = info.state;
  state->generation = info.generation;
  return 0;
}
