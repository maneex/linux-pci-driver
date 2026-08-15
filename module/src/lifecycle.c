#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "../include/pci.h"
#include "device.h"

static const struct pci_device_id pci_id_table[] = {
    {PCI_DEVICE(VELOCITOR_PCI_VENDORID, VELOCITOR_PCI_DEVICEID)},
    {
        0,
    }};

// https : // www.kernel.org/doc/html/v6.0/PCI/pci.html

static int velocitor_release(struct pci_dev *dev,
                             enum InitialisationState state, int ret) {
  struct Device *device = pci_get_drvdata(dev);

  // Disable the device from generating IRQs
  if (INIT_STATE_PROBED != state)
    pci_disable_device(dev);

  // Release the IRQ (free_irq())
  // Stop all DMA activity
  // Release DMA buffers (both streaming and coherent)
  // Unregister from other subsystems (e.g. scsi or netdev)

  switch (state) {
  case INIT_STATE_COMPLETE:
    fallthrough;

  case INIT_STATE_IOMAP: // Release MMIO/IOP resources
    if (NULL != device->bar0)
      pci_iounmap(dev, device->bar2);
    if (NULL != device->bar2)
      pci_iounmap(dev, device->bar2);
    if (NULL != device->bar4)
      pci_iounmap(dev, device->bar4);
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
  u32 magic = readl(device->bar0 + BAR0_MAGIC_OFFSET);
  if (BAR0_MAGIC_VALUE != magic) {
    dev_err(&dev->dev,
            "velocitor.bar0: invalid magic: got 0x%08x, expected 0x%08x\n",
            magic, BAR0_MAGIC_VALUE);
    return -ENODEV;
  }
  pr_info("velocitor.bar0: magic verified");

  // Check SCRATCH
  const u32 scratches[] = {0x42000042, 0x12345678};
  for (int i = 0; i < 2; ++i) {
    writel(scratches[i], device->bar0 + BAR0_SCRATCH_OFFSET);
    u32 res = readl(device->bar0 + BAR0_SCRATCH_OFFSET);
    if (~scratches[i] != res) {
      dev_err(&dev->dev,
              "velocitor.bar0: invalid scratch: got 0x%08x, expected 0x%08x\n",
              res, ~scratches[i]);
      return -ENODEV;
    }
  }

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
  pr_info("velocitor.[%s].pci_probe: device found", pci_name(dev));

  //  Enable the device
  if ((err = pci_enable_device(dev)))
    return velocitor_release(dev, INIT_STATE_PROBED, err);
  pr_info("velocitor.pci_probe[%s]: device enabled", pci_name(dev));

  // Request MMIO/IOP resources
  if ((err = pci_request_regions(dev, KBUILD_MODNAME)))
    return velocitor_release(dev, INIT_STATE_ENABLED, err);
  pr_info("velocitor.[%s].pci_probe: request regions as %s", pci_name(dev),
          KBUILD_MODNAME);

  if ((err = initialize_bar0(dev, device)))
    return velocitor_release(dev, INIT_STATE_IOMAP, err);

  // Set the DMA mask size (for both coherent and streaming DMA)
  // Allocate and initialize shared control data (pci_allocate_coherent())
  // Access device configuration space (if needed)
  // Register IRQ handler (request_irq())
  // Initialize non-PCI (i.e. LAN/SCSI/etc parts of the chip)
  // Enable DMA/processing engines

  pr_info("velocitor.[%s].pci_probe: innitialisation complete", pci_name(dev));

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
  pr_info("Loading velocitor driver");
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

  pr_info("Unloading velocitor driver");
  pci_unregister_driver(&velocitor_pci_driver);
}

module_init(init_);
module_exit(exit_);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anichini Perceval");
MODULE_DESCRIPTION("A dummy accelerator driver study");
