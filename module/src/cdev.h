#ifndef VELOCITOR_CHRDEV_H
#define VELOCITOR_CHRDEV_H

// Linux headers.
#include <linux/cdev.h>
#include <linux/pci.h>

struct velocitor_cdev {
  dev_t devno;

  struct cdev cdev;

  struct class *class;

  struct device *device;

  unsigned long usageFlags;
};

int velocitor_cdev_initialize(struct pci_dev *dev);

#endif // not VELOCITOR_CHRDEV_H
