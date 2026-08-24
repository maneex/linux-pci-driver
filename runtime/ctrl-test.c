/*
 * runtime/ctrl-test.c -- layer 2 of spec section 13.1 for the control plane.
 *
 * Driven by devtools/guest-ctrl-test.sh, which owns the environment; this
 * owns the checks that need an ioctl, so it has to be one process: the
 * exclusive open of section 10.2 allows a single one at a time, and the
 * crash test below needs a handle to survive across a firmware generation.
 *
 * The oracles are the ones the driver cannot forge. Node 0 is 112 MiB
 * allocatable and node 1 is 128 because the fixed aperture is taken off
 * node 0 (spec 3.2) -- an asymmetry the spec exposes rather than corrects,
 * and which comes from the model's geometry, not from anything the driver
 * computes.
 */

// C headers.
#include <stdio.h>
#include <string.h>
#include <time.h>

// Velocitor headers.
#include "velocitor/velocitor.h"

#define NODE0_ALLOCATABLE (112ull * 1024 * 1024)
#define NODE1_ALLOCATABLE (128ull * 1024 * 1024)
#define BLOCK 4096ull

static int passed;
static int failed;

static void check(int ok, const char *what) {
  if (ok) {
    ++passed;
    printf("  ok    %s\n", what);
  } else {
    ++failed;
    printf("  FAIL  %s\n", what);
  }
}

static void nap(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
  nanosleep(&ts, NULL);
}

int main(void) {
  struct velocitor_device_info devices[VEL_MAX_DEVICES] = {};

  /* ------------------------------------------------- 1. enumeration ---- */

  int found = velocitor_scan(devices, VEL_MAX_DEVICES);
  check(1 == found, "sysfs enumeration finds exactly one device");
  if (1 != found)
    return 2;

  check(0 == strcmp(devices[0].devname, "velocitor0"), "devname is velocitor0");
  check(2 == devices[0].topology.nodes, "topology says two nodes");
  check(2 == devices[0].topology.engines, "topology says two engines");

  /* ------------------------------------------------------- 2. open ----- */

  struct velocitor_device *dev = NULL;
  check(0 == velocitor_open(&devices[0], &dev), "open succeeds");
  if (NULL == dev)
    return 2;

  /* Spec 10.2: a single open at a time, the second one is refused. */
  struct velocitor_device *second = NULL;
  check(-16 == velocitor_open(&devices[0], &second), "second open is -EBUSY");

  /* ------------------------------------------------------- 3. STAT ----- */

  struct velocitor_stats stats = {};
  check(0 == velocitor_stats(dev, &stats), "STAT answers");
  check(NODE0_ALLOCATABLE == stats.nodes[0].free,
        "node 0 offers 112 MiB, aperture deducted (spec 3.2)");
  check(NODE1_ALLOCATABLE == stats.nodes[1].free, "node 1 offers 128 MiB");
  check(0 == stats.live_handles, "no live handle yet");

  /* ------------------------------------------ 4. one allocation -------- */

  struct velocitor_handle *block = NULL;
  check(0 == velocitor_alloc(dev, BLOCK, 0, 0, &block),
        "ALLOC of 4096 bytes on node 0");

  check(0 == velocitor_stats(dev, &stats), "STAT after the allocation");
  check(1 == stats.live_handles, "one live handle");
  check(NODE0_ALLOCATABLE - BLOCK == stats.nodes[0].free,
        "node 0 lost exactly what was asked for");
  check(NODE1_ALLOCATABLE == stats.nodes[1].free, "node 1 untouched");

  /* ------------------------------------------------------- 5. FREE ----- */

  check(0 == velocitor_free(block), "FREE of that handle");
  check(0 == velocitor_stats(dev, &stats), "STAT after the free");
  check(0 == stats.live_handles, "no live handle again");

  /*
   * Spec 7.2 and 14: FREE always invalidates the handle, and the bump
   * allocator gives nothing back. Both halves are visible here, and a real
   * allocator would make this one check fail on purpose.
   */
  check(NODE0_ALLOCATABLE - BLOCK == stats.nodes[0].free,
        "the memory did not come back (bump allocator, spec 14)");

  /* --------------------------------------------- 6. refusals ----------- */

  struct velocitor_handle *nope = NULL;
  check(-22 == velocitor_alloc(dev, 0, 0, 0, &nope),
        "ALLOC of zero bytes is -EINVAL");
  check(-22 == velocitor_alloc(dev, BLOCK, 99, 0, &nope),
        "ALLOC on a node that does not exist is -EINVAL");

  /* --------------------------- 7. crash, recovery, stale handle -------- */
  /*
   * The point of the whole exercise. A handle made before a firmware crash
   * must not silently name someone else's block afterwards: the allocator
   * restarts at each generation (spec 6.5), so handle 1 of generation 2 is
   * not handle 1 of generation 1, and nothing about the value says so.
   *
   * The descriptor survives the crash, which is what section 10.1 promises
   * and what makes the recovery observable from an application at all
   * (spec 12, item 6).
   */

  struct velocitor_state before = {};
  check(0 == velocitor_state(dev, &before), "INFO before the crash");
  check(VEL_DEVICE_INFO_READY == before.state, "state is READY");

  struct velocitor_handle *doomed = NULL;
  check(0 == velocitor_alloc(dev, BLOCK, 0, 0, &doomed),
        "ALLOC that will not survive the crash");

  FILE *inject = fopen("/sys/kernel/debug/velocitor/0000:00:03.0/inject_error",
                       "we");
  if (NULL == inject) {
    check(0, "debugfs inject_error is reachable");
    return failed;
  }
  fprintf(inject, "4\n"); /* VEL_ERR_INJECT_FW_CRASH, spec 9 */
  fclose(inject);

  struct velocitor_state after = {};
  int spins = 0;
  do {
    nap();
    velocitor_state(dev, &after);
  } while ((after.generation == before.generation) && (++spins < 50));

  check(after.generation > before.generation,
        "GENERATION moved on, the firmware rebooted (spec 6.5)");
  check(VEL_DEVICE_INFO_READY == after.state, "state is READY again");

  /* The descriptor was never closed, and still works. */
  check(0 == velocitor_stats(dev, &stats), "the same fd still serves STAT");
  check(0 == stats.live_handles, "the new generation starts with no handle");

  check(-116 == velocitor_free(doomed),
        "FREE of a handle from the previous generation is -ESTALE");

  struct velocitor_handle *fresh = NULL;
  check(0 == velocitor_alloc(dev, BLOCK, 0, 0, &fresh),
        "ALLOC works again after the recovery");
  check(0 == velocitor_free(fresh), "and so does FREE");

  check(0 == velocitor_close(dev), "close");

  printf("\npassed %d, failed %d\n", passed, failed);
  return failed;
}
