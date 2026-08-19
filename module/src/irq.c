// Linux headers.
#include <linux/pci.h>
#include <linux/remoteproc.h>

// Do not reorder.
#include "remoteproc_internal.h"

// Velotcitor headers.
#include <velocitor_hw.h>

// Driver headers
#include "device.h"
#include "error.h"
#include "irq.h"

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

static irqreturn_t irq_queue_event(int irq, void *data) {
  const struct velocitor_vring *vring = data;
  const struct velocitor_dev *device = pci_get_drvdata(vring->dev);

  if (NULL != device->rproc.handle) {
    irqreturn_t res = rproc_vq_interrupt(device->rproc.handle, vring->notifyid);
    trace_velocitor_irq_vring(vring->vector, vring->notifyid,
                              IRQ_HANDLED == res);
  }
  return IRQ_HANDLED;
}

static irqreturn_t irq_error_event(int irq, void *data) {
  struct pci_dev *dev = data;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  struct velocitor_error error = {};
  velocitor_error_read(device, &error);

  // Unlatch interrupt, trace and report.
  writel(BIT(VEL_IRQ_VEC_ERROR), device->bar0 + VEL_REG_IRQ_ACK);
  trace_velocitor_error(&error);
  if (NULL != device->rproc.handle)
    rproc_report_crash(device->rproc.handle, RPROC_FATAL_ERROR);
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

  struct velocitor_dev *device = pci_get_drvdata(dev);
  for (unsigned i = 0; i < VEL_VRINGS_COUNT; ++i) {
    if ((err = devm_request_irq(
             &dev->dev, pci_irq_vector(dev, device->vrings[i].vector),
             irq_queue_event, 0, device->vrings[i].vector_name,
             device->vrings + i)))
      return err;
  }

  if ((err = devm_request_irq(&dev->dev, pci_irq_vector(dev, 5),
                              irq_error_event, 0, "velocitor-err", dev)))
    return err;
  return 0;
}
