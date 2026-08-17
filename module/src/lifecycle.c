// Linux headers.
#include <linux/dcache.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

// Velocitor headers.
#include "../../qemu-device/velocitor_hw.h"

// Driver headers.
#include "debugfs.h"
#include "device.h"
#include "irq.h"

static const struct pci_device_id pci_id_table[] = {
    {PCI_DEVICE(VEL_PCI_VENDOR_ID, VEL_PCI_DEVICE_ID)},
    {
        0,
    }};
MODULE_DEVICE_TABLE(pci, pci_id_table);

static struct dentry *velocitor_debugfs_root = 0;

// https : // www.kernel.org/doc/html/v6.0/PCI/pci.html

static int initialize_bar0(struct pci_dev *dev, struct Device *device) {
  // Get mapping
  device->bar0 = pcim_iomap_region(dev, 0, KBUILD_MODNAME);
  if (IS_ERR(device->bar0))
    return PTR_ERR(device->bar0);

  // Check MAGIC
  u32 magic = readl(device->bar0 + VEL_REG_MAGIC);
  if (VEL_MAGIC != magic) {
    dev_err(&dev->dev,
            "velocitor.bar0: invalid magic: got 0x%08x, expected 0x%08x\n",
            magic, VEL_MAGIC);
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
  device->bar2 = pcim_iomap_region(dev, 2, KBUILD_MODNAME);
  if (IS_ERR(device->bar2))
    return PTR_ERR(device->bar2);

  return 0;
}

static int initialize_dma_engine(struct pci_dev *dev, struct Device *device) {
  int err = 0;
  pci_set_master(dev);
  if ((err = dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(VEL_DMA_BITS))))
    return err;
  dev_info(&dev->dev, "dma: width %d bits", VEL_DMA_BITS);
  return 0;
}

static int velocitor_pci_probe(struct pci_dev *dev,
                               const struct pci_device_id *device_id) {
  int err = 0;
  struct Device *device =
      devm_kzalloc(&(dev->dev), sizeof(struct Device), GFP_KERNEL);
  if (NULL == device)
    return -ENOMEM;

  if ((err = devm_mutex_init(&dev->dev, &device->lock_counters)))
    return err;

  pci_set_drvdata(dev, device);
  dev_info(&dev->dev, "probe: device found");

  velocitor_debugfs_initialize(dev, velocitor_debugfs_root);

  //  Enable the device
  if ((err = pcim_enable_device(dev)))
    return err;
  dev_info(&dev->dev, "probe: device enabled");

  // Request MMIO/IOP resources
  if ((err = initialize_bar0(dev, device)))
    return err;
  if ((err = initialize_bar2(dev, device)))
    return err;

  // Set the DMA mask size (for both coherent and streaming DMA)
  // Allocate and initialize shared control data (pci_allocate_coherent())
  if ((err = initialize_dma_engine(dev, device)))
    return err;

  // Access device configuration space (if needed)
  // Register IRQ handler (request_irq())
  if ((err = velocitor_irq_initialize(dev)))
    return err;

  // Initialize non-PCI (i.e. LAN/SCSI/etc parts of the chip)
  // Enable DMA/processing engines

  dev_info(&dev->dev, "probe: initialisation complete");

  return 0;
}

static void velocitor_pci_remove(struct pci_dev *dev) {}

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
  pr_info("velocitor: loading driver\n");
  velocitor_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
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

  pr_info("velocitor: unloading driver\n");
  pci_unregister_driver(&velocitor_pci_driver);
  debugfs_remove_recursive(velocitor_debugfs_root);
}

module_init(init_);
module_exit(exit_);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anichini Perceval");
MODULE_DESCRIPTION("A dummy accelerator driver study");
