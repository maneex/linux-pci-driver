// Linux headers.
#include <linux/pci.h>

// Velotcitor headers.
#include <velocitor_hw.h>

// Driver headers
#include "device.h"
#include "irq.h"

// This is the one translation unit that emits the tracepoint symbols, so the
// macro goes *before* the include -- define_trace.h tests it as it is read.
// Any other file wanting trace_velocitor_irq() includes trace.h without it.
#define CREATE_TRACE_POINTS
#include "trace.h"

static void velocitor_irq_release(void *data) { pci_free_irq_vectors(data); }

static irqreturn_t irq_config_event(int irq, void *data) {
  struct pci_dev *dev = data;
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  u32 status = readl(device->bar0 + VEL_REG_FW_STATUS);
  writel(BIT(VEL_IRQ_VEC_CONFIG), device->bar0 + VEL_REG_IRQ_ACK);
  trace_velocitor_irq_cfg(status);
  return IRQ_HANDLED;
}

static irqreturn_t irq_queue0_event(int irq, void *dev) {
  trace_velocitor_irq(1);
  return IRQ_HANDLED;
}

static irqreturn_t irq_queue1_event(int irq, void *dev) {
  trace_velocitor_irq(2);
  return IRQ_HANDLED;
}

static irqreturn_t irq_queue2_event(int irq, void *dev) {
  trace_velocitor_irq(3);
  return IRQ_HANDLED;
}

static irqreturn_t irq_queue3_event(int irq, void *dev) {
  trace_velocitor_irq(4);
  return IRQ_HANDLED;
}

static irqreturn_t irq_error_event(int irq, void *data) {
  struct pci_dev *dev = data;
  const struct velocitor_dev *device = pci_get_drvdata(dev);

  trace_velocitor_irq(5);
  // FIXME ! Do something :)

  // Unlatch interrupt.
  writel(BIT(VEL_IRQ_VEC_ERROR), device->bar0 + VEL_REG_IRQ_ACK);
  return IRQ_HANDLED;
}

// https://docs.kernel.org/PCI/msi-howto.html
// https://kernel-internals.org/interrupts/threaded-irq/
int velocitor_irq_initialize(struct pci_dev *dev) {
  int err = 0;

  if ((err = pci_alloc_irq_vectors(dev, VEL_MSIX_VECTORS, VEL_MSIX_VECTORS,
                                   PCI_IRQ_MSIX)) < 0)
    return err;

  if ((err = devm_add_action_or_reset(&dev->dev, velocitor_irq_release, dev)))
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
