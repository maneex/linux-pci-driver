// Velocitor headers.
#include <velocitor_hw.h>

// Project headers.
#include "counters.h"
#include "device.h"

void velocitor_reset_counters(struct pci_dev *dev) {
  struct Device *device = pci_get_drvdata(dev);

  // Thou shalt not reset while I'm reading.
  mutex_lock(&device->lock_counters);
  writel(1, device->bar0 + VEL_REG_CNT_RESET);
  mutex_unlock(&device->lock_counters);
}

void velocitor_read_counters(struct pci_dev *dev, struct counters *counters) {
  struct Device *device = pci_get_drvdata(dev);

  mutex_lock(&device->lock_counters);

  // Snapshot counters..
  writel(1, device->bar0 + VEL_REG_CNT_SNAP);

  // Read values.
  counters->db_rx = readl(device->bar0 + VEL_REG_CNT_DB_RX);
  counters->notify_tx = readl(device->bar0 + VEL_REG_CNT_NOTIFY_TX);
  counters->notify_coalesced =
      readl(device->bar0 + VEL_REG_CNT_NOTIFY_COALESCED);
  counters->notify_dropped = readl(device->bar0 + VEL_REG_CNT_NOTIFY_DROPPED);
  counters->desc = readl(device->bar0 + VEL_REG_CNT_DESC);
  counters->gemm = readl(device->bar0 + VEL_REG_CNT_GEMM);
  counters->dma_rd = readl(device->bar0 + VEL_REG_CNT_DMA_RD);
  counters->dma_wr = readl(device->bar0 + VEL_REG_CNT_DMA_WR);
  counters->bytes_rd_lo = readl(device->bar0 + VEL_REG_CNT_BYTES_RD_LO);
  counters->bytes_rd_hi = readl(device->bar0 + VEL_REG_CNT_BYTES_RD_HI);
  counters->bytes_wr_lo = readl(device->bar0 + VEL_REG_CNT_BYTES_WR_LO);
  counters->bytes_wr_hi = readl(device->bar0 + VEL_REG_CNT_BYTES_WR_HI);
  counters->win_move = readl(device->bar0 + VEL_REG_CNT_WIN_MOVE);
  counters->far_access = readl(device->bar0 + VEL_REG_CNT_FAR_ACCESS);
  counters->err_desc = readl(device->bar0 + VEL_REG_CNT_ERR_DESC);
  counters->err_range = readl(device->bar0 + VEL_REG_CNT_ERR_RANGE);
  counters->stall_e0 = readl(device->bar0 + VEL_REG_CNT_STALL_E0);
  counters->stall_e1 = readl(device->bar0 + VEL_REG_CNT_STALL_E1);
  counters->cycles_e0 = readl(device->bar0 + VEL_REG_CNT_CYCLES_E0);
  counters->cycles_e1 = readl(device->bar0 + VEL_REG_CNT_CYCLES_E1);

  mutex_unlock(&device->lock_counters);
}
