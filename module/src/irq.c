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

irqreturn_t irq_config_event(int irq, void *dev) {
  trace_velocitor_irq(0);
  return IRQ_HANDLED;
}

irqreturn_t irq_queue0_event(int irq, void *dev) {
  trace_velocitor_irq(1);
  return IRQ_HANDLED;
}

irqreturn_t irq_queue1_event(int irq, void *dev) {
  trace_velocitor_irq(2);
  return IRQ_HANDLED;
}

irqreturn_t irq_queue2_event(int irq, void *dev) {
  trace_velocitor_irq(3);
  return IRQ_HANDLED;
}

irqreturn_t irq_queue3_event(int irq, void *dev) {
  trace_velocitor_irq(4);
  return IRQ_HANDLED;
}

irqreturn_t irq_error_event(int irq, void *data) {
  struct pci_dev *dev = data;
  struct Device *device = pci_get_drvdata(dev);

  trace_velocitor_irq(5);
  // FIXME ! Do something :)

  // Unlatch interrupt.
  writel(BIT(VEL_IRQ_VEC_ERROR), device->bar0 + VEL_REG_IRQ_ACK);
  return IRQ_HANDLED;
}
