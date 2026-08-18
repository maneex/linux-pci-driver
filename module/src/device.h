#ifndef VELOCITOR_DEVICE_H
#define VELOCITOR_DEVICE_H

// Linux headers.
#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>

struct velocitor_dma_buf {
  void *cpu;
  dma_addr_t dma;
};

struct velocitor_dev {
  void __iomem *bar0;
  void __iomem *bar2;

  struct dentry *debugfs;

  struct {
    struct {
      u16 minor;
      u16 major;
    } version;
  } identity;

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
    struct rproc *handle;
    struct velocitor_dma_buf rsc;
    struct velocitor_dma_buf vrings[4];
  } rproc;
};

#endif // not VELOCITOR_DEVICE_H
