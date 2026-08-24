#ifndef VELOCITOR_RUNTIME_H
#define VELOCITOR_RUNTIME_H

// C headers.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Velocitor headers.
#include <velocitor.h>

#ifdef __cplusplus
extern "C" {
#endif // __cpplusplus

#define VEL_EXPORT __attribute__((visibility("default")))

#define VEL_DEVNAME_MAX 64

/// Description of a velocitor device.
struct velocitor_device_info {
  /// True if device is present
  bool present;

  unsigned int minor;

  /// Node name, as uevent publishes it: the device is /dev/<devname>.
  char devname[VEL_DEVNAME_MAX];

  // Firmware version
  struct {
    unsigned int minor;
    unsigned int major;
  } firmware_version;

  // Topology
  struct {
    // Memory size.
    size_t memory;

    // Nuumber of nodes.
    unsigned int nodes;

    // Number of engines.
    unsigned int engines;
  } topology;
};

/// An open device. Opaque: its definition lives inside the library.
struct velocitor_device;
struct velocitor_handle;

VEL_EXPORT int velocitor_scan(struct velocitor_device_info *devices,
                              unsigned int count);

/// Open a device.
VEL_EXPORT int velocitor_open(const struct velocitor_device_info *info,
                              struct velocitor_device **device);

VEL_EXPORT int velocitor_close(struct velocitor_device *device);

VEL_EXPORT const struct velocitor_device_info *
velocitor_info(const struct velocitor_device *device);

struct velocitor_stats {
  unsigned int live_handles;

  struct {
    uint64_t capacity;
    uint64_t free;
  } nodes[VEL_UAPI_NODES];
};

VEL_EXPORT int velocitor_stats(const struct velocitor_device *device,
                               struct velocitor_stats *stats);

VEL_EXPORT int velocitor_alloc(const struct velocitor_device *device,
                               uint64_t size, uint32_t node, uint32_t type,
                               struct velocitor_handle **handle);

VEL_EXPORT int velocitor_free(struct velocitor_handle *handle);

#ifdef __cplusplus
}
#endif // __cpplusplus

#endif // not VELOCITOR_RUNTIME_H
