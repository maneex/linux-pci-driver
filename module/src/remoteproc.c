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
  for (int i = 0; i < VEL_VRINGS_COUNT; ++i) {
    mem = rproc_mem_entry_init(rproc->dev.parent, device->vring.vqs[i].mem.cpu,
                               device->vring.vqs[i].mem.dma, VEL_VRING_SIZE,
                               FW_RSC_ADDR_ANY, NULL, NULL, "vdev%dvring%d",
                               i / 2, i % 2);
    if (NULL == mem)
      return -ENOMEM;
    rproc_add_carveout(rproc, mem);
  }

  return 0;
}

static int velocitor_rproc_walk_rsc_table(struct velocitor_dev *device,
                                          struct rproc *rproc) {
  if (NULL == rproc->cached_table)
    return -EIO;

  int vring_index = 0;
  for (int i = 0; i < rproc->cached_table->num; ++i) {
    struct fw_rsc_hdr *header =
        (void *)rproc->cached_table + rproc->cached_table->offset[i];
    if (RSC_VDEV != header->type)
      continue;

    struct fw_rsc_vdev *vdev = (void *)header + sizeof(struct fw_rsc_hdr);
    for (u8 j = 0; j < vdev->num_of_vrings; ++j) {
      if (vring_index >= VEL_VRINGS_COUNT)
        return -EINVAL;
      device->vring.vqs[vring_index++].notifyid = vdev->vring[j].notifyid;
    }
  }
  return VEL_VRINGS_COUNT == vring_index ? 0 : -EINVAL;
}

static int velocitor_rproc_stop(struct rproc *rproc) {
  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  writel(1, device->bar0 + VEL_REG_RESET);
  writel(0, device->bar0 + VEL_REG_RSC_VALID);

  return 0;
}

static int velocitor_rproc_start(struct rproc *rproc) {
  int err = 0;
  struct pci_dev *dev = to_pci_dev(rproc->dev.parent);
  struct velocitor_dev *device = pci_get_drvdata(dev);

  dev_info(&dev->dev, "rproc: starting");

  velocitor_vring_invalidate(dev);

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
    err = VEL_ERR_FW_HEADER == readl(device->bar0 + VEL_REG_ERR_CODE)
              ? -ENODEV
              : -ETIMEDOUT;
    goto error;
  }

  if (VEL_FW_STATUS_RUNNING != status) {
    dev_info(&dev->dev, "rproc: unable to start remote processor");
    err = -EIO;
    goto error;
  }

  // Update generation.
  device->rproc.generation = readl(device->bar0 + VEL_REG_GENERATION);

  // Where the firmware put its trace ring (spec 6.6).  Readable only now:
  // before the load, device memory still holds the reset pattern.
  device->dtrace.da =
      readl(device->bar2 + VEL_FW_HDR_DA + VEL_FW_HDR_OFF_TRACE_DA);

  // Check ABI.
  if ((VEL_FW_ABI != readl(device->bar0 + VEL_REG_FW_ABI)) ||
      (VEL_TRACE_ENTRY !=
       readl(device->bar2 + device->dtrace.da + VEL_TRACE_OFF_ENTRY_SIZE))) {
    err = -ENODEV;
    goto error;
  }

  // Initialize vrings
  if ((err = velocitor_rproc_walk_rsc_table(device, rproc))) {
    goto error;
  }
  velocitor_vring_activate(dev);
  return 0;

error:
  velocitor_rproc_stop(rproc);
  return err;
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

  if ((err = devm_rproc_add(&dev->dev, device->rproc.handle)))
    return err;

  dev_info(&dev->dev, "rproc: booting");
  if ((err = rproc_boot(device->rproc.handle)))
    return err;

  return 0;
}
