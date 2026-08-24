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
  return 0;
}
