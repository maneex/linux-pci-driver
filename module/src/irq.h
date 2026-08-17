#ifndef VELOCITOR_IRQ_H
#define VELOCITOR_IRQ_H

#include <linux/irq.h>

int velocitor_irq_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_IRQ_H
