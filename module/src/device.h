#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

// Linux headers.
#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>

// Driver headers.
#include "cdev.h"
#include "ctrl.h"
#include "data.h"
#include "device-trace.h"
#include "dma.h"
#include "handles.h"
#include "identity.h"
#include "vring.h"

struct velocitor_dev {
  /// BAR 0 address.
  void __iomem *bar0;
  /// BAR 2 address.
  void __iomem *bar2;

  /// DebugFS root for this device.
  struct dentry *debugfs;

  /// Device identity
  struct velocitor_identity identity;

  /// Device counters.
  struct {
    struct mutex lock;
  } counters;

  /// DMA engine.
  struct velocitor_dma dma;

  /// Window
  struct {
    /// Base address of the window.
    size_t base;

    /// Lock protecting window access.
    struct mutex lock;
  } window;

  /// VRings
  struct {
    /// Lock protecting vqs.
    struct mutex lock;

    /// Queues.
    struct velocitor_vring vqs[VEL_VRINGS_COUNT];
  } vring;

  /// Device trace structure.
  struct velocitor_dtrace dtrace;

  /// RPMSG Control bus.
  struct velocitor_ctrl ctrl;

  /// VIRTIO Data bus.
  struct velocitor_data data;

  /// Allocation tables.
  struct velocitor_handles handles;

  /// Remote proc.
  struct {
    /// Remote proc handle.
    struct rproc *handle;

    /// Current generation.
    u32 generation;

    /// Pointer to RSC table.
    struct velocitor_dma_buf rsc;
  } rproc;

  /// Character device.
  struct velocitor_cdev cdev;
};

#endif // not VELOCITOR_DEVICE_H
