// Linux headers.
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "device.h"
#include "dma.h"
#include "trace.h"

// https:/www.kernel.org/doc/html/v6.8/core-api/dma-api-howto.html

static void velocitor_dma_release(void *data) {
  struct pci_dev *dev = data;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  dma_free_coherent(&dev->dev, VEL_HOST_POOL_SIZE, device->dma.cpu_addr,
                    device->dma.handle);
  device->dma.cpu_addr = NULL;
  device->dma.handle = 0;
}

//-EIO, -EDTIMEOUT
static int velocitor_dma_dbg_(struct pci_dev *dev, u32 dir, u32 offset,
                              u32 pool_offset, u32 len, u32 *err_code) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  // Sanity check.
  u32 end = 0;
  if ((check_add_overflow(pool_offset, len, &end)) ||
      (end > VEL_HOST_POOL_SIZE))
    return -EINVAL;

  // Write target address
  u64 target = (device->dma.handle + pool_offset);
  mutex_lock(&device->dma.dbg_lock);
  writel(lower_32_bits(target), device->bar0 + VEL_REG_DBG_DMA_ADDR_LO);
  writel(upper_32_bits(target), device->bar0 + VEL_REG_DBG_DMA_ADDR_HI);

  // Write source offset, and length.
  writel(offset, device->bar0 + VEL_REG_DBG_DMA_DEV);
  writel(len, device->bar0 + VEL_REG_DBG_DMA_LEN);

  // Start transfer.
  writel(dir, device->bar0 + VEL_REG_DBG_DMA_CTL);

  // Wait for completion.
  u32 status = 0;
  int res = readl_poll_timeout(
      device->bar0 + VEL_REG_DBG_DMA_STATUS, status,
      ((VEL_DMA_STATUS_DONE == status) || (VEL_DMA_STATUS_ERROR == status)), 20,
      1000000);

  if (status == VEL_DMA_STATUS_ERROR) {
    if (NULL != err_code)
      *err_code = readl(device->bar0 + VEL_REG_ERR_CODE);
    res = -EIO;
  } else {
    if (NULL != err_code)
      *err_code = 0;
  }

  mutex_unlock(&device->dma.dbg_lock);

  trace_velocitor_dma_dbg(dir, offset, pool_offset, len, res, status,
                          NULL == err_code ? 0 : *err_code);
  return res;
}

static ssize_t velocitor_dma_debugfs_pool_read(struct file *file,
                                               char __user *buf, size_t len,
                                               loff_t *ppos) {
  struct pci_dev *dev = file->private_data;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  return simple_read_from_buffer(buf, len, ppos, device->dma.cpu_addr,
                                 VEL_HOST_POOL_SIZE);
}

static ssize_t velocitor_dma_debugfs_pool_write(struct file *file,
                                                const char __user *buf,
                                                size_t len, loff_t *ppos) {
  struct pci_dev *dev = file->private_data;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  return simple_write_to_buffer(device->dma.cpu_addr, VEL_HOST_POOL_SIZE, ppos,
                                buf, len);
}

static const struct file_operations velocitor_dma_debugfs_pool_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .read = velocitor_dma_debugfs_pool_read,
    .write = velocitor_dma_debugfs_pool_write,
    .llseek = default_llseek,
};

static ssize_t velocitor_dma_debugfs_ctrl_write(struct file *file,
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

static const struct file_operations velocitor_dma_debugfs_ctrl_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .write = velocitor_dma_debugfs_ctrl_write,
    .llseek = noop_llseek,
};

int velocitor_dma_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  if ((err = devm_mutex_init(&dev->dev, &device->dma.dbg_lock)))
    return err;

  pci_set_master(dev);
  if ((err = dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(VEL_DMA_BITS))))
    return err;
  dev_info(&dev->dev, "dma: width %d bits", VEL_DMA_BITS);

  device->dma.cpu_addr = dma_alloc_coherent(&dev->dev, VEL_HOST_POOL_SIZE,
                                            &device->dma.handle, GFP_KERNEL);
  if (NULL == device->dma.cpu_addr)
    return -ENOMEM;

  if ((err = devm_add_action_or_reset(&dev->dev, velocitor_dma_release, dev)))
    return err;

  debugfs_create_file_size("dma_pool", 0600, device->debugfs, dev,
                           &velocitor_dma_debugfs_pool_fops,
                           VEL_HOST_POOL_SIZE);

  debugfs_create_file("dma_ctrl", 0200, device->debugfs, dev,
                      &velocitor_dma_debugfs_ctrl_fops);

  return 0;
}

int velocitor_dma_dbg_write(struct pci_dev *dev, u32 offset, u32 pool_offset,
                            u32 len, u32 *err_code) {
  return velocitor_dma_dbg_(dev, VEL_DBG_DMA_H2D, offset, pool_offset, len,
                            err_code);
}

int velocitor_dma_dbg_read(struct pci_dev *dev, u32 offset, u32 pool_offset,
                           u32 len, u32 *err_code) {
  return velocitor_dma_dbg_(dev, VEL_DBG_DMA_D2H, offset, pool_offset, len,
                            err_code);
}
