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

int velocitor_dma_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

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
