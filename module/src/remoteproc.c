// Linux headers.
#include <linux/iopoll.h>
#include <linux/pci.h>
#include <linux/remoteproc.h>
#include <linux/virtio_ring.h>

// The generic ELF ops are exported, but only declared in the remoteproc core's
// private header; the Makefile adds drivers/remoteproc to the include path.
#include "remoteproc_internal.h"

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "remoteproc.h"

static int velocitor_rproc_prepare(struct rproc *rproc) {
  struct rproc_mem_entry *mem = NULL;
  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  // heap
  mem = rproc_mem_entry_init(rproc->dev.parent, NULL, 0,
                             VEL_MEM_SIZE - VEL_APERTURE_SIZE,
                             VEL_APERTURE_SIZE, NULL, NULL, "heap");
  if (NULL == mem)
    return -ENOMEM;
  rproc_add_carveout(rproc, mem);

  // vrings.
  for (int i = 0; i < 4; ++i) {
    mem = rproc_mem_entry_init(rproc->dev.parent, device->rproc.vrings[i].cpu,
                               device->rproc.vrings[i].dma, VEL_VRING_SIZE,
                               FW_RSC_ADDR_ANY, NULL, NULL, "vdev%dvring%d",
                               i / 2, i % 2);
    if (NULL == mem)
      return -ENOMEM;
    rproc_add_carveout(rproc, mem);
  }

  return 0;
}

static int velocitor_rproc_start(struct rproc *rproc) {
  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  dev_info(&dev->dev, "rproc: starting");

  writel(lower_32_bits(device->rproc.rsc.dma),
         device->bar0 + VEL_REG_RSC_ADDR_LO);
  writel(upper_32_bits(device->rproc.rsc.dma),
         device->bar0 + VEL_REG_RSC_ADDR_HI);
  writel(rproc->table_sz, device->bar0 + VEL_REG_RSC_LEN);
  writel(1, device->bar0 + VEL_REG_RSC_VALID);

  writel(0, device->bar0 + VEL_REG_RESET);

  u32 status = 0;
  if (readl_poll_timeout(device->bar0 + VEL_REG_FW_STATUS, status,
                         status >= VEL_FW_STATUS_RUNNING, 20, 20000)) {
    dev_err(&dev->dev, "rproc: error while starting firmware.");
    return VEL_ERR_FW_HEADER == readl(device->bar0 + VEL_REG_ERR_CODE)
               ? -ENODEV
               : -ETIMEDOUT;
  }

  dev_info(&dev->dev, "rproc: firmware %s",
           VEL_FW_STATUS_RUNNING == status ? "started" : "error");
  return VEL_FW_STATUS_RUNNING == status ? 0 : -EIO;
}

static int velocitor_rproc_stop(struct rproc *rproc) {
  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  writel(1, device->bar0 + VEL_REG_RESET);
  writel(0, device->bar0 + VEL_REG_RSC_VALID);

  return 0;
}

static void velocitor_rproc_kick(struct rproc *rproc, int notifyid) {
  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  writel(notifyid, device->bar0 + VEL_REG_DOORBELL);
}

static void *velocitor_rproc_datova(struct rproc *rproc, u64 da, size_t len,
                                    bool *is_iomem) {
  u64 end = 0;
  if ((check_add_overflow(da, len, &end)) || (end > VEL_APERTURE_SIZE))
    return NULL;

  if (NULL != is_iomem)
    *is_iomem = true;

  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);
  return (void __force *)(device->bar2 + da);
}

static struct resource_table *
velocitor_rproc_find_loaded_rsc_table(struct rproc *rproc,
                                      const struct firmware *fw) {
  if (rproc->table_sz > PAGE_SIZE)
    return NULL;

  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  return device->rproc.rsc.cpu;
}

static void velocitor_rproc_vring_initialize(struct velocitor_dev *device,
                                             u32 idx) {
  struct vring vr;
  vring_init(&vr, VEL_VRING_NUM, device->rproc.vrings[idx].cpu,
             VEL_VRING_ALIGN);

  writel(idx, device->bar0 + VEL_REG_VQ_SELECT);

  dma_addr_t desc = device->rproc.vrings[idx].dma;
  dma_addr_t avail = desc + ((void *)vr.avail - device->rproc.vrings[idx].cpu);
  dma_addr_t used = desc + ((void *)vr.used - device->rproc.vrings[idx].cpu);
  writel(lower_32_bits(desc), device->bar0 + VEL_REG_VQ_DESC_LO);
  writel(upper_32_bits(desc), device->bar0 + VEL_REG_VQ_DESC_HI);
  writel(lower_32_bits(avail), device->bar0 + VEL_REG_VQ_AVAIL_LO);
  writel(upper_32_bits(avail), device->bar0 + VEL_REG_VQ_AVAIL_HI);
  writel(lower_32_bits(used), device->bar0 + VEL_REG_VQ_USED_LO);
  writel(upper_32_bits(used), device->bar0 + VEL_REG_VQ_USED_HI);
  writel(VEL_VRING_NUM, device->bar0 + VEL_REG_VQ_NUM);
  writel(idx + 1, device->bar0 + VEL_REG_VQ_MSIX_VECTOR);
  writel(1, device->bar0 + VEL_REG_VQ_ENABLE);
}

static const struct rproc_ops velocitor_rproc_ops = {
    .prepare = velocitor_rproc_prepare,
    .unprepare = NULL,
    .start = velocitor_rproc_start,
    .stop = velocitor_rproc_stop,
    .attach = NULL,
    .detach = NULL,
    .kick = velocitor_rproc_kick,
    .da_to_va = velocitor_rproc_datova,
    .parse_fw = rproc_elf_load_rsc_table,
    .handle_rsc = NULL,
    .find_loaded_rsc_table = velocitor_rproc_find_loaded_rsc_table,
    .get_loaded_rsc_table = NULL,
    .load = rproc_elf_load_segments,
    .sanity_check = rproc_elf_sanity_check,
    .get_boot_addr = rproc_elf_get_boot_addr,
    .panic = NULL,
    .coredump = NULL};

int velocitor_remoteproc_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  device->rproc.handle = devm_rproc_alloc(
      &dev->dev, "velocitor", &velocitor_rproc_ops, "velocitor-fw.elf", 0);
  if (!device->rproc.handle)
    return -ENOMEM;

  device->rproc.handle->auto_boot = 0;

  device->rproc.rsc.cpu = dmam_alloc_coherent(
      &dev->dev, PAGE_SIZE, &device->rproc.rsc.dma, GFP_KERNEL);
  if (NULL == device->rproc.rsc.cpu)
    return -ENOMEM;

  for (int i = 0; i < 4; ++i) {
    device->rproc.vrings[i].cpu = dmam_alloc_coherent(
        &dev->dev, VEL_VRING_SIZE, &device->rproc.vrings[i].dma, GFP_KERNEL);
    if (NULL == device->rproc.vrings[i].cpu)
      return -ENOMEM;
  }

  if ((err = devm_rproc_add(&dev->dev, device->rproc.handle)))
    return err;

  dev_info(&dev->dev, "rproc: booting");
  if ((err = rproc_boot(device->rproc.handle)))
    return err;

  for (int i = 0; i < 4; ++i)
    velocitor_rproc_vring_initialize(device, i);

  return 0;
}
