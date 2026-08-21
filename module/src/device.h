#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

// Linux headers.
#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>

// Driver headers.
#include "ctrl.h"
#include "device-trace.h"
#include "dma.h"
#include "identity.h"
#include "vring.h"

struct velocitor_dev {
  void __iomem *bar0;
  void __iomem *bar2;

  struct dentry *debugfs;

  struct velocitor_identity identity;

  struct {
    struct mutex lock;
  } counters;

  struct {
    void *cpu_addr;
    dma_addr_t handle;
    struct mutex dbg_lock;
  } dma;

  struct {
    size_t base;
    struct mutex lock;
  } window;

  struct {
    struct mutex lock;
    struct velocitor_vring vqs[VEL_VRINGS_COUNT];
  } vring;

  struct velocitor_dtrace dtrace;

  // Control plane, spec 7. The table outlives the rpmsg channel on purpose;
  // see the comment on struct velocitor_ctrl.
  struct velocitor_ctrl ctrl;

  struct {
    struct rproc *handle;
    u32 generation;
    struct velocitor_dma_buf rsc;
  } rproc;
};

#endif // not VELOCITOR_DEVICE_H
