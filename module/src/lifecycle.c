#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "../../qemu-device/velocitor_hw.h"
#include "device.h"
#include "irq.h"

static const struct pci_device_id pci_id_table[] = {
    {PCI_DEVICE(VEL_PCI_VENDOR_ID, VEL_PCI_DEVICE_ID)},
    {
        0,
    }};

// https : // www.kernel.org/doc/html/v6.0/PCI/pci.html

static int velocitor_release(struct pci_dev *dev,
                             enum InitialisationState state, int ret) {
  struct Device *device = pci_get_drvdata(dev);

  // Release the IRQ (free_irq())
  // Stop all DMA activity
  // Release DMA buffers (both streaming and coherent)
  // Unregister from other subsystems (e.g. scsi or netdev)

  switch (state) {
  case INIT_STATE_COMPLETE:
    fallthrough;

  case INIT_STATE_IRQ:
    pci_free_irq_vectors(dev);
    fallthrough;

  case INIT_STATE_DMA:
    pci_clear_master(dev);
    fallthrough;

  case INIT_STATE_IOMAP: // Release MMIO/IOP resources
    if (NULL != device->bar0)
      pci_iounmap(dev, device->bar0);
    if (NULL != device->bar2)
      pci_iounmap(dev, device->bar2);
    pci_release_regions(dev);
    fallthrough;

  case INIT_STATE_ENABLED:
    pci_disable_device(dev);
    fallthrough;

  case INIT_STATE_PROBED:
    break;
  }

  return ret;
}

static int initialize_bar0(struct pci_dev *dev, struct Device *device) {
  // Get mapping
  device->bar0 = pci_iomap(dev, 0, 0);
  if (NULL == device->bar0)
    return -ENOMEM;

  // Check MAGIC
  u32 magic = readl(device->bar0 + VEL_REG_MAGIC);
  if (VEL_FW_MAGIC != magic) {
    dev_err(&dev->dev,
            "velocitor.bar0: invalid magic: got 0x%08x, expected 0x%08x\n",
            magic, VEL_FW_MAGIC);
    return -ENODEV;
  }
  dev_info(&dev->dev, "bar0: magic verified");

  // Check SCRATCH
  const u32 scratches[] = {0x42000042, 0x12345678};
  for (int i = 0; i < 2; ++i) {
    writel(scratches[i], device->bar0 + VEL_REG_SCRATCH);
    u32 res = readl(device->bar0 + VEL_REG_SCRATCH);
    if (~scratches[i] != res) {
      dev_err(&dev->dev,
              "velocitor.bar0: invalid scratch: got 0x%08x, expected 0x%08x\n",
              res, ~scratches[i]);
      return -ENODEV;
    }
  }

  u32 version = readl(device->bar0 + VEL_REG_VERSION);
  dev_info(&dev->dev, "bar0: device version %d.%d", version >> 16,
           version & 0xffff);

  return 0;
}

static int initialize_bar2(struct pci_dev *dev, struct Device *device) {
  device->bar2 = pci_iomap(dev, 2, 0);
  if (NULL == device->bar2)
    return -ENOMEM;

  return 0;
}

static int initialize_dma_engine(struct pci_dev *dev, struct Device *device) {
  int err = 0;
  pci_set_master(dev);
  if ((err = dma_set_mask_and_coherent(&dev->dev, VEL_DMA_BITS)))
    return err;
  dev_info(&dev->dev, "dma: width %d bits", VEL_DMA_BITS);
  return 0;
}

// https://docs.kernel.org/PCI/msi-howto.html
// https://kernel-internals.org/interrupts/threaded-irq/
static int initialize_irq_handlers(struct pci_dev *dev, struct Device *device) {
  int err = 0;
  if ((6 != pci_alloc_irq_vectors(dev, 6, 6, PCI_IRQ_MSIX)))
    return err;

  // FIXME! IRQ handler affinity ?
  // FIXME! Threaded IRQ ?
  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 0),
                              irq_config_event, 0, "velocitor-cfg", dev)))
    return err;
  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 1),
                              irq_queue0_event, 0, "velocitor-q0", dev)))
    return err;
  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 2),
                              irq_queue1_event, 0, "velocitor-q1", dev)))
    return err;
  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 3),
                              irq_queue2_event, 0, "velocitor-q2", dev)))
    return err;
  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 4),
                              irq_queue3_event, 0, "velocitor-q3", dev)))
    return err;

  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 5),
                              irq_error_event, 0, "velocitor-err", dev)))
    return err;
  return 0;
}

static int velocitor_pci_probe(struct pci_dev *dev,
                               const struct pci_device_id *device_id) {
  int err = 0;
  struct Device *device =
      devm_kzalloc(&(dev->dev), sizeof(struct Device), GFP_KERNEL);
  if (NULL == device)
    return -ENOMEM;
  pci_set_drvdata(dev, device);
  dev_info(&dev->dev, "probe: device found");

  //  Enable the device
  if ((err = pci_enable_device(dev)))
    return velocitor_release(dev, INIT_STATE_PROBED, err);
  dev_info(&dev->dev, "probe: device enabled");

  // Request MMIO/IOP resources
  if ((err = pci_request_regions(dev, KBUILD_MODNAME)))
    return velocitor_release(dev, INIT_STATE_ENABLED, err);
  dev_info(&dev->dev, "probe: request regions as %s", KBUILD_MODNAME);

  if ((err = initialize_bar0(dev, device)))
    return velocitor_release(dev, INIT_STATE_IOMAP, err);
  if ((err = initialize_bar2(dev, device)))
    return velocitor_release(dev, INIT_STATE_IOMAP, err);

  // Set the DMA mask size (for both coherent and streaming DMA)
  // Allocate and initialize shared control data (pci_allocate_coherent())
  if ((err = initialize_dma_engine(dev, device)))
    return velocitor_release(dev, INIT_STATE_DMA, err);

  // Access device configuration space (if needed)
  // Register IRQ handler (request_irq())
  if ((err = initialize_irq_handlers(dev, device)))
    return velocitor_release(dev, INIT_STATE_DMA, err);

  // Initialize non-PCI (i.e. LAN/SCSI/etc parts of the chip)
  // Enable DMA/processing engines

  dev_info(&dev->dev, "probe: initialisation complete");

  return 0;
}

static void velocitor_pci_remove(struct pci_dev *dev) {
  velocitor_release(dev, INIT_STATE_COMPLETE, 0);
}

static struct pci_driver velocitor_pci_driver = {
    .name = "velocitor",
    .id_table = pci_id_table,
    .probe = velocitor_pci_probe,
    .remove = velocitor_pci_remove,
    .suspend = NULL,
    .resume = NULL,

    .shutdown = NULL,
    .sriov_configure = NULL,
    .sriov_set_msix_vec_count = NULL,
    .sriov_get_vf_total_msix = NULL,
    .err_handler = NULL,
    .groups = NULL,
    .dev_groups = NULL};

static int __init init_(void) {
  pr_info("velocitor: loading driver");
  return pci_register_driver(&velocitor_pci_driver);
}

static void __exit exit_(void) {
  // Disable the device from generating IRQs
  // Release the IRQ (free_irq())
  // Stop all DMA activity
  // Release DMA buffers (both streaming and coherent)
  // Unregister from other subsystems (e.g. scsi or netdev)
  // Release MMIO/IOP resources
  // Disable the device

  pr_info("velocitor: unloading driver");
  pci_unregister_driver(&velocitor_pci_driver);
}

module_init(init_);
module_exit(exit_);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anichini Perceval");
MODULE_DESCRIPTION("A dummy accelerator driver study");
