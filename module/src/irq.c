// Linux headers.
#include <linux/pci.h>

// Velotcitor headers.
#include <velocitor_hw.h>

// Driver headers
#include "device.h"
#include "irq.h"

irqreturn_t irq_config_event(int irq, void *dev) { return IRQ_HANDLED; }
irqreturn_t irq_queue0_event(int irq, void *dev) { return IRQ_HANDLED; }
irqreturn_t irq_queue1_event(int irq, void *dev) { return IRQ_HANDLED; }
irqreturn_t irq_queue2_event(int irq, void *dev) { return IRQ_HANDLED; }
irqreturn_t irq_queue3_event(int irq, void *dev) { return IRQ_HANDLED; }

irqreturn_t irq_error_event(int irq, void *data) {
  struct pci_dev *dev = data;
  struct Device *device = pci_get_drvdata(dev);

  dev_warn(&dev->dev, "irq: error latched");
  // FIXME ! Do something :)

  // Unlatch interrupt.
  writel(BIT(VEL_IRQ_VEC_ERROR), device->bar0 + VEL_REG_IRQ_ACK);
  return IRQ_HANDLED;
}
