// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "counters.h"
#include "debugfs.h"

static int velocitor_debugfs_counters_show(struct seq_file *s, void *unused) {
  struct pci_dev *dev = s->private;
  struct counters counters = {};

  velocitor_read_counters(dev, &counters);
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
DEFINE_SHOW_ATTRIBUTE(velocitor_debugfs_counters);

static int velocitor_debugfs_counters_reset(void *dev, u64 val) {
  if (1 != val)
    return -EINVAL;
  velocitor_reset_counters(dev);
  return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(velocitor_debugfs_counters_reset_fops, NULL,
                         velocitor_debugfs_counters_reset, "%llu\n");

static int velocitor_debugfs_inject_error(void *dev, u64 cmd) {
  if (cmd > 255)
    return -EINVAL;

  struct Device *device = pci_get_drvdata(dev);
  writel(cmd, device->bar0 + VEL_REG_ERR_INJECT);
  return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(velocitor_debugfs_inject_error_fops, NULL,
                         velocitor_debugfs_inject_error, "%llu\n");

static void velocitor_debugfs_release(void *root) {
  debugfs_remove_recursive(root);
}

int velocitor_debugfs_initialize(struct pci_dev *dev, struct dentry *root) {
  int err = 0;
  struct Device *device = pci_get_drvdata(dev);

  device->debugfs = debugfs_create_dir(pci_name(dev), root);
  if ((err = devm_add_action_or_reset(&dev->dev, velocitor_debugfs_release,
                                      device->debugfs)))
    return err;

  debugfs_create_file("counters", 0444, device->debugfs, dev,
                      &velocitor_debugfs_counters_fops);
  debugfs_create_file_unsafe("counters_reset", 0200, device->debugfs, dev,
                             &velocitor_debugfs_counters_reset_fops);
  debugfs_create_file_unsafe("inject_error", 0200, device->debugfs, dev,
                             &velocitor_debugfs_inject_error_fops);
  return 0;
}
