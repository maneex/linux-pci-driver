// Linux headers.
#include <linux/virtio_ring.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Module headers.
#include "device.h"
#include "vring.h"

static int velocitor_vring_debugfs_show(struct seq_file *s, void *unused) {
  struct velocitor_dev *device = pci_get_drvdata((struct pci_dev *)s->private);

  mutex_lock(&device->vring.lock);
  for (int i = 0; i < VEL_VRINGS_COUNT; ++i) {
    seq_printf(s, "vdev%dvring%d:\n", i / 2, i % 2);
    seq_printf(s, "  irq:       %d\n", device->vring.vqs[i].irq);
    seq_printf(s, "  vector:    %d\n", device->vring.vqs[i].vector);
    seq_printf(s, "  notifyid:  %d\n", device->vring.vqs[i].notifyid);
    seq_printf(s, "  address:   cpu=%px dma=%pad\n",
               (void *)device->vring.vqs[i].mem.cpu,
               (void *)device->vring.vqs[i].mem.dma);
    writel(i, device->bar0 + VEL_REG_VQ_SELECT);
    seq_printf(s, "  vq_enable: %d\n\n",
               readl(device->bar0 + VEL_REG_VQ_ENABLE));
  }

  mutex_unlock(&device->vring.lock);
  return 0;
}
DEFINE_SHOW_ATTRIBUTE(velocitor_vring_debugfs);

static int velocitor_vring_initialize_(struct pci_dev *dev,
                                       struct velocitor_vring *vring, int idx) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  vring->dev = dev;
  vring->index = idx;
  vring->vector = idx + 1;
  vring->notifyid = -1;
  vring->vector_name = devm_kasprintf(&dev->dev, GFP_KERNEL, "velocitor-v%dr%d",
                                      idx / 2, idx % 2);
  if (NULL == vring->vector_name)
    return -ENOMEM;

  vring->mem.cpu = dmam_alloc_coherent(&dev->dev, VEL_VRING_SIZE,
                                       &vring->mem.dma, GFP_KERNEL);
  if (NULL == vring->mem.cpu)
    return -ENOMEM;

  debugfs_create_file("vring", 0444, device->debugfs, dev,
                      &velocitor_vring_debugfs_fops);

  return 0;
}

static void velocitor_vring_activate_(const struct velocitor_dev *device,
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

int velocitor_vring_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  if ((err = devm_mutex_init(&dev->dev, &device->vring.lock)))
    return err;

  for (int i = 0; i < VEL_VRINGS_COUNT; ++i)
    if ((err = velocitor_vring_initialize_(dev, device->vring.vqs + i, i)))
      break;

  return err;
}

void velocitor_vring_activate(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  mutex_lock(&device->vring.lock);
  for (int i = 0; i < VEL_VRINGS_COUNT; ++i)
    velocitor_vring_activate_(device, device->vring.vqs + i);
  mutex_unlock(&device->vring.lock);
}

void velocitor_vring_invalidate(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);
  mutex_lock(&device->vring.lock);
  for (int i = 0; i < VEL_VRINGS_COUNT; ++i) {
    device->vring.vqs[i].notifyid = -1;
    memset(device->vring.vqs[i].mem.cpu, 0x00, VEL_VRING_SIZE);
  }
  mutex_unlock(&device->vring.lock);
}
