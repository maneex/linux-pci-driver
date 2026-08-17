#ifndef VELOCITOR_IRQ_H
#define VELOCITOR_IRQ_H

#include <linux/irq.h>

irqreturn_t irq_config_event(int irq, void *dev);
irqreturn_t irq_queue0_event(int irq, void *dev);
irqreturn_t irq_queue1_event(int irq, void *dev);
irqreturn_t irq_queue2_event(int irq, void *dev);
irqreturn_t irq_queue3_event(int irq, void *dev);
irqreturn_t irq_error_event(int irq, void *data);

#endif // not VELOCITOR_IRQ_H
