// Linux headers.
#include <linux/uaccess.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "counters.h"
#include "debugfs.h"
#include "dma.h"
#include "window.h"

/**
 * DebugFS / Counters.
 */

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

/**
 * DebugFS / Inject error
 */
static int velocitor_debugfs_inject_error(void *dev, u64 cmd) {
  if (cmd > 255)
    return -EINVAL;

  struct velocitor_dev *device = pci_get_drvdata(dev);
  writel(cmd, device->bar0 + VEL_REG_ERR_INJECT);
  return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(velocitor_debugfs_inject_error_fops, NULL,
                         velocitor_debugfs_inject_error, "%llu\n");

static void velocitor_debugfs_release(void *root) {
  debugfs_remove_recursive(root);
}

/**
 * DebugFS / DMA
 */

static ssize_t velocitor_debugfs_dma_pool_read(struct file *file,
                                               char __user *buf, size_t len,
                                               loff_t *ppos) {
  struct pci_dev *dev = file->private_data;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  return simple_read_from_buffer(buf, len, ppos, device->dma.cpu_addr,
                                 VEL_HOST_POOL_SIZE);
}

static ssize_t velocitor_debugfs_dma_pool_write(struct file *file,
                                                const char __user *buf,
                                                size_t len, loff_t *ppos) {
  struct pci_dev *dev = file->private_data;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  return simple_write_to_buffer(device->dma.cpu_addr, VEL_HOST_POOL_SIZE, ppos,
                                buf, len);
}

static const struct file_operations velocitor_debugfs_dma_pool_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .read = velocitor_debugfs_dma_pool_read,
    .write = velocitor_debugfs_dma_pool_write,
    .llseek = default_llseek,
};

static ssize_t velocitor_debugfs_dma_ctrl_write(struct file *file,
                                                const char __user *buf,
                                                size_t len, loff_t *ppos) {
  struct pci_dev *dev = file->private_data;
  char line[64] = {};
  char dir[4] = {};
  u32 offset = 0;
  u32 pool_offset = 0;
  u32 length = 0;
  u32 err_code = VEL_ERR_NONE;
  int res = 0;

  if (0 == len || len >= sizeof(line))
    return -EINVAL;
  if (copy_from_user(line, buf, len))
    return -EFAULT;

  // %i takes the base from the prefix, so 0x1000 and 4096 both work.
  if (4 != sscanf(line, "%3s %i %i %i", dir, &offset, &pool_offset, &length))
    return -EINVAL;

  if (0 == strcmp(dir, "h2d"))
    res = velocitor_dma_dbg_write(dev, offset, pool_offset, length, &err_code);
  else if (0 == strcmp(dir, "d2h"))
    res = velocitor_dma_dbg_read(dev, offset, pool_offset, length, &err_code);
  else
    return -EINVAL;

  // A short write would make the caller retry the rest of its line.
  return res ? res : len;
}

static const struct file_operations velocitor_debugfs_dma_ctrl_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .write = velocitor_debugfs_dma_ctrl_write,
    .llseek = noop_llseek,
};

/**
 * DebugFS / Window
 */
static ssize_t velocitor_debugfs_mem_read(struct file *file, char __user *buf,
                                          size_t len, loff_t *offset) {
  struct pci_dev *dev = file->private_data;
  int res = 0;
  if (*offset >= VEL_MEM_SIZE)
    return 0;

  void *page = kmalloc(PAGE_SIZE, GFP_KERNEL);
  if (NULL == page) {
    res = -ENOMEM;
    goto out;
  }

  len = min_t(size_t, len, VEL_MEM_SIZE - *offset);
  len = min_t(size_t, len, PAGE_SIZE);

  res = velocitor_window_read(dev, page, *offset, len);
  if (0 == res) {
    if (!copy_to_user(buf, page, len))
      *offset += len;
    else
      res = -EFAULT;
  }

out:
  kfree(page);
  return res < 0 ? res : len;
}

static const struct file_operations velocitor_debugfs_mem_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .read = velocitor_debugfs_mem_read,
    .llseek = default_llseek,
};

/**
 * DebugFS / Initialize.
 */

int velocitor_debugfs_initialize(struct pci_dev *dev, struct dentry *root) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

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

  debugfs_create_file_size("dma_pool", 0600, device->debugfs, dev,
                           &velocitor_debugfs_dma_pool_fops,
                           VEL_HOST_POOL_SIZE);

  debugfs_create_file("dma_ctrl", 0200, device->debugfs, dev,
                      &velocitor_debugfs_dma_ctrl_fops);

  debugfs_create_file_size("mem", 0400, device->debugfs, dev,
                           &velocitor_debugfs_mem_fops, VEL_MEM_SIZE);

  return 0;
}
