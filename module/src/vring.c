// Linux headers.
#include <linux/virtio_ring.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Module headers.
#include "device.h"
#include "vring.h"

static int velocitor_vrings_initialize_(struct pci_dev *dev,
                                        struct velocitor_vring *vring,
                                        int idx) {
  vring->dev = dev;
  vring->index = idx;
  vring->vector = idx + 1;
  vring->notifyid = -1;
  vring->name =
      devm_kasprintf(&dev->dev, GFP_KERNEL, "vdev%dvring%d", idx / 2, idx % 2);
  if (NULL == vring->name)
    return -ENOMEM;

  vring->mem.cpu = dmam_alloc_coherent(&dev->dev, VEL_VRING_SIZE,
                                       &vring->mem.dma, GFP_KERNEL);
  if (NULL == vring->mem.cpu)
    return -ENOMEM;

  return 0;
}

static void velocitor_vrings_activate_(const struct velocitor_dev *device,
                                       const struct velocitor_vring *vring) {
  struct vring vr;
  vring_init(&vr, VEL_VRING_NUM, vring->mem.cpu, VEL_VRING_ALIGN);

  writel(vring->index, device->bar0 + VEL_REG_VQ_SELECT);

  dma_addr_t desc = vring->mem.dma;
  dma_addr_t avail = desc + ((void *)vr.avail - vring->mem.cpu);
  dma_addr_t used = desc + ((void *)vr.used - vring->mem.cpu);
  writel(lower_32_bits(desc), device->bar0 + VEL_REG_VQ_DESC_LO);
  writel(upper_32_bits(desc), device->bar0 + VEL_REG_VQ_DESC_HI);
  writel(lower_32_bits(avail), device->bar0 + VEL_REG_VQ_AVAIL_LO);
  writel(upper_32_bits(avail), device->bar0 + VEL_REG_VQ_AVAIL_HI);
  writel(lower_32_bits(used), device->bar0 + VEL_REG_VQ_USED_LO);
  writel(upper_32_bits(used), device->bar0 + VEL_REG_VQ_USED_HI);
  writel(VEL_VRING_NUM, device->bar0 + VEL_REG_VQ_NUM);
  writel(vring->vector, device->bar0 + VEL_REG_VQ_MSIX_VECTOR);
  writel(1, device->bar0 + VEL_REG_VQ_ENABLE);
}

int velocitor_vrings_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);
  for (int i = 0; i < VEL_VRINGS_COUNT; ++i)
    if ((err = velocitor_vrings_initialize_(dev, device->vrings + i, i)))
      return err;
  return 0;
}

void velocitor_vrings_activate(struct pci_dev *dev) {
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  for (int i = 0; i < VEL_VRINGS_COUNT; ++i)
    velocitor_vrings_activate_(device, device->vrings + i);
}
