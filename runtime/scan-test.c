// C headers.
#include <stdio.h>

// Velocitor headers.
#include "velocitor/velocitor.h"

int main(void) {
  struct velocitor_device_info devices[VEL_MAX_DEVICES] = {};

  int found = velocitor_scan(devices, VEL_MAX_DEVICES);
  if (found < 0) {
    printf("scan failed: %d\n", found);
    return 1;
  }

  printf("found %d device(s)\n", found);
  for (int i = 0; i < found; ++i)
    printf("  /dev/%s (minor %u): version %u.%u, %u node(s), %u engine(s), "
           "%zu byte(s)\n",
           devices[i].devname, devices[i].minor,
           devices[i].firmware_version.major, devices[i].firmware_version.minor,
           devices[i].topology.nodes, devices[i].topology.engines,
           devices[i].topology.memory);

  if (found > 0) {
    struct velocitor_device *device = NULL;

    int err = velocitor_open(&devices[0], &device);
    printf("open %s: %d\n", devices[0].devname, err);
    if (0 == err) {
      const struct velocitor_device_info *info = velocitor_info(device);
      printf("device describes /dev/%s, %u node(s)\n", info->devname,
             info->topology.nodes);
      struct velocitor_stats st = {};
      int sr = velocitor_stats(device, &st);
      printf("stats: %d, live %u, node0 free %llu, node1 free %llu\n", sr,
             st.live_handles, (unsigned long long)st.nodes[0].free,
             (unsigned long long)st.nodes[1].free);

      struct velocitor_handle *h = NULL;
      int a = velocitor_alloc(device, 4096, 0, 0, &h);
      printf("alloc 4096 on node 0: %d\n", a);
      velocitor_stats(device, &st);
      printf("after alloc: live %u, node0 free %llu\n", st.live_handles,
             (unsigned long long)st.nodes[0].free);

      if (0 == a)
        printf("free: %d\n", velocitor_free(h));

      velocitor_stats(device, &st);
      printf("after free:  live %u, node0 free %llu\n", st.live_handles,
             (unsigned long long)st.nodes[0].free);

      struct velocitor_handle *bad = NULL;
      printf("alloc 0 bytes: %d\n", velocitor_alloc(device, 0, 0, 0, &bad));
      printf("alloc on node 99: %d\n",
             velocitor_alloc(device, 4096, 99, 0, &bad));

      printf("close: %d\n", velocitor_close(device));
      printf("close(NULL): %d\n", velocitor_close(NULL));
    }
  }
  return 0;
}
