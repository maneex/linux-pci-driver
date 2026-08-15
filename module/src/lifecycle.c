#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "../include/pci.h"

static struct pci_device_id pci_id_table[] = {
    {PCI_DEVICE(VELOCITOR_PCI_VENDORID, VELOCITOR_PCI_DEVICEID)},
    {
        0,
    }};

// https : // www.kernel.org/doc/html/v6.0/PCI/pci.html

static int velocitor_pci_probe(struct pci_dev *dev,
                               const struct pci_device_id *device_id) {

  pr_info("velocitor.pci_probe[%s]: device found", pci_name(dev));

  //  Enable the device
  int err = pcim_enable_device(dev);
  if (err)
    return err;

  pr_info("velocitor.pci_probe[%s]: device enabled", pci_name(dev));

  // Request MMIO/IOP resources
  // Set the DMA mask size (for both coherent and streaming DMA)
  // Allocate and initialize shared control data (pci_allocate_coherent())
  // Access device configuration space (if needed)
  // Register IRQ handler (request_irq())
  // Initialize non-PCI (i.e. LAN/SCSI/etc parts of the chip)
  // Enable DMA/processing engines

  return 0;
}

static void velocitor_pci_remove(struct pci_dev *dev) {
  // Disable the device from generating IRQs
  // Release the IRQ (free_irq())
  // Stop all DMA activity
  // Release DMA buffers (both streaming and coherent)
  // Unregister from other subsystems (e.g. scsi or netdev)
  // Release MMIO/IOP resources
  // Disable the device : automatically done by pcim
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
