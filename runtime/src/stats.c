// C headers.
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

// Project headers
#include "private.h"

int velocitor_stats(const struct velocitor_device *device,
                    struct velocitor_stats *stats) {
  if ((NULL == device) || (-1 == device->fd) || (NULL == stats))
    return -EINVAL;

  struct velocitor_ioctl_stats raw = {};
  if (ioctl(device->fd, VEL_IOC_STATS, &raw) < 0)
    return -errno;

  memset(stats, 0, sizeof(*stats));
  stats->live_handles = raw.live_handles;
  for (unsigned int i = 0; i < VEL_UAPI_NODES; ++i) {
    stats->nodes[i].capacity = raw.nodes[i].capacity;
    stats->nodes[i].free = raw.nodes[i].free;
  }

  return 0;
}
