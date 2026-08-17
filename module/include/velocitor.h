#ifndef VELOCITOR_H
#define VELOCITOR_H

// Linux headers.
#include <linux/types.h>

struct counters {
  u32 db_rx;
  u32 notify_tx;
  u32 notify_coalesced;
  u32 notify_dropped;
  u32 desc;
  u32 gemm;
  u32 dma_rd;
  u32 dma_wr;
  u32 bytes_rd_lo;
  u32 bytes_rd_hi;
  u32 bytes_wr_lo;
  u32 bytes_wr_hi;
  u32 win_move;
  u32 far_access;
  u32 err_desc;
  u32 err_range;
  u32 stall_e0;
  u32 stall_e1;
  u32 cycles_e0;
  u32 cycles_e1;
};

#endif // not VELOCITOR_H
