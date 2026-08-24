/*
 * UAPI of /dev/velocitor
 */
#ifndef VELOCITOR_H
#define VELOCITOR_H

// Linux headers.
#include <linux/ioctl.h>
#include <linux/types.h>

struct counters {
  __u32 db_rx;
  __u32 notify_tx;
  __u32 notify_coalesced;
  __u32 notify_dropped;
  __u32 desc;
  __u32 gemm;
  __u32 dma_rd;
  __u32 dma_wr;
  __u32 bytes_rd_lo;
  __u32 bytes_rd_hi;
  __u32 bytes_wr_lo;
  __u32 bytes_wr_hi;
  __u32 win_move;
  __u32 far_access;
  __u32 err_desc;
  __u32 err_range;
  __u32 stall_e0;
  __u32 stall_e1;
  __u32 cycles_e0;
  __u32 cycles_e1;
};

#define VEL_DEVICE_INFO_DOWN 0
#define VEL_DEVICE_INFO_BOOTING 1
#define VEL_DEVICE_INFO_READY 2
#define VEL_DEVICE_INFO_CRASHED 3

struct velocitor_ioctl_handle {
  __u32 addr;
  __u32 node;
  __u32 generation;
};

struct velocitor_ioctl_info {
  __u32 state;
  __u32 generation;
};

struct velocitor_ioctl_alloc {
  __u64 size;
  __u32 node;
  __u32 dtype;
  struct velocitor_ioctl_handle handle;
};

struct velocitor_ioctl_stat_node {
  u64 capacity;
  u64 free;
};

struct velocitor_ioctl_stats {
  u32 live_handles;
  struct velocitor_ioctl_stat_node nodes[VEL_NODES];
};

#define VEL_DRV_MAGIC 'v'
#define VEL_DRV_IOCTL_BASE 0x30
#define VEL_IOC_INFO                                                           \
  _IOR(VEL_DRV_MAGIC, VEL_DRV_IOCTL_BASE + 0, struct velocitor_ioctl_info)
#define VEL_IOC_STATS                                                          \
  _IOR(VEL_DRV_MAGIC, VEL_DRV_IOCTL_BASE + 1, struct counters)
#define VEL_IOC_ALLOC_DEV                                                      \
  _IOWR(VEL_DRV_MAGIC, VEL_DRV_IOCTL_BASE + 2, unsigned long)
#define VEL_IOC_FREE_DEV                                                       \
  _IOW(VEL_DRV_MAGIC, VEL_DRV_IOCTL_BASE + 3, unsigned long)
#endif // not VELOCITOR_H
