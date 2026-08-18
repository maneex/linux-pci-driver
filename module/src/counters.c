// Velocitor headers.
#include <velocitor_hw.h>

// Project headers.
#include "counters.h"
#include "device.h"

static int velocitor_counters_debugfs_show(struct seq_file *s, void *unused) {
  struct pci_dev *dev = s->private;
  struct counters counters = {};

  velocitor_counters_read(dev, &counters);
  seq_printf(s, "db_rx                %u\n", counters.db_rx);
  seq_printf(s, "notify_tx            %u\n", counters.notify_tx);
  seq_printf(s, "notify_coalesced     %u\n", counters.notify_coalesced);
  seq_printf(s, "notify_dropped       %u\n", counters.notify_dropped);
  seq_printf(s, "desc                 %u\n", counters.desc);
  seq_printf(s, "gemm                 %u\n", counters.gemm);
  seq_printf(s, "dma_rd               %u\n", counters.dma_rd);
  seq_printf(s, "dma_wr               %u\n", counters.dma_wr);
  seq_printf(s, "bytes_rd_lo          %u\n", counters.bytes_rd_lo);
  seq_printf(s, "bytes_rd_hi          %u\n", counters.bytes_rd_hi);
  seq_printf(s, "bytes_rd             %llu\n",
             ((u64)counters.bytes_rd_hi << 32) | counters.bytes_rd_lo);
  seq_printf(s, "bytes_wr_lo          %u\n", counters.bytes_wr_lo);
  seq_printf(s, "bytes_wr_hi          %u\n", counters.bytes_wr_hi);
  seq_printf(s, "bytes_wr             %llu\n",
             ((u64)counters.bytes_wr_hi << 32) | counters.bytes_wr_lo);
  seq_printf(s, "win_move             %u\n", counters.win_move);
  seq_printf(s, "far_access           %u\n", counters.far_access);
  seq_printf(s, "err_desc             %u\n", counters.err_desc);
  seq_printf(s, "err_range            %u\n", counters.err_range);
  seq_printf(s, "stall_e0             %u\n", counters.stall_e0);
  seq_printf(s, "stall_e1             %u\n", counters.stall_e1);
  seq_printf(s, "cycles_e0            %u\n", counters.cycles_e0);
  seq_printf(s, "cycles_e1            %u\n", counters.cycles_e1);
  return 0;
}
DEFINE_SHOW_ATTRIBUTE(velocitor_counters_debugfs);

static int velocitor_counters_debugfs_reset(void *dev, u64 val) {
  if (1 != val)
    return -EINVAL;
  velocitor_counters_reset(dev);
  return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(velocitor_counters_debugfs_reset_fops, NULL,
                         velocitor_counters_debugfs_reset, "%llu\n");

void velocitor_counters_reset(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  // Thou shalt not reset while I'm reading.
  mutex_lock(&device->counters.lock);
  writel(1, device->bar0 + VEL_REG_CNT_RESET);
  mutex_unlock(&device->counters.lock);
}

void velocitor_counters_read(struct pci_dev *dev, struct counters *counters) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  mutex_lock(&device->counters.lock);

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

  mutex_unlock(&device->counters.lock);
}

int velocitor_counters_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  int err = 0;
  if ((err = devm_mutex_init(&dev->dev, &device->counters.lock)))
    return err;

  debugfs_create_file("counters", 0444, device->debugfs, dev,
                      &velocitor_counters_debugfs_fops);

  debugfs_create_file_unsafe("counters_reset", 0200, device->debugfs, dev,
                             &velocitor_counters_debugfs_reset_fops);

  return 0;
}
