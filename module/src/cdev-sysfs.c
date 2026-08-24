// Linux headers.
#include <linux/device.h>
#include <linux/sysfs.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "cdev-sysfs.h"
#include "device.h"

// https://docs.kernel.org/filesystems/sysfs.html

/*
 * VERSION as the register holds it, major in the high half and minor in the
 * low one -- not "0.6".
 *
 * A dotted string is two values in one file, which sysfs.rst forbids and
 * which, more to the point, stops a reader from getting it in one scanf: the
 * conversion stops at the dot and the minor half is lost in silence. What the
 * driver read is a single value; splitting it for a human to look at is the
 * caller's business, not the attribute's.
 */
static ssize_t version_show(struct device *dev, struct device_attribute *attr,
                            char *buf) {
  const struct velocitor_dev *device = dev_get_drvdata(dev);

  return sysfs_emit(buf, "0x%08x\n",
                    ((u32)device->identity.version.major << 16) |
                        device->identity.version.minor);
}
static DEVICE_ATTR_RO(version);

static ssize_t nodes_show(struct device *dev, struct device_attribute *attr,
                          char *buf) {
  const struct velocitor_dev *device = dev_get_drvdata(dev);

  return sysfs_emit(buf, "%u\n", device->identity.topology.nodes);
}
static DEVICE_ATTR_RO(nodes);

static ssize_t engines_show(struct device *dev, struct device_attribute *attr,
                            char *buf) {
  const struct velocitor_dev *device = dev_get_drvdata(dev);

  return sysfs_emit(buf, "%u\n", device->identity.topology.engines);
}
static DEVICE_ATTR_RO(engines);

static ssize_t mem_size_show(struct device *dev, struct device_attribute *attr,
                             char *buf) {
  const struct velocitor_dev *device = dev_get_drvdata(dev);

  return sysfs_emit(buf, "%u\n", device->identity.topology.mem_size);
}
static DEVICE_ATTR_RO(mem_size);

static ssize_t caps_show(struct device *dev, struct device_attribute *attr,
                         char *buf) {
  const struct velocitor_dev *device = dev_get_drvdata(dev);

  return sysfs_emit(buf, "0x%08x\n", device->identity.caps);
}
static DEVICE_ATTR_RO(caps);

static struct attribute *velocitor_cdev_attrs[] = {
    &dev_attr_version.attr,  &dev_attr_nodes.attr, &dev_attr_engines.attr,
    &dev_attr_mem_size.attr, &dev_attr_caps.attr,  NULL};

static const struct attribute_group velocitor_cdev_group = {
    .attrs = velocitor_cdev_attrs};

const struct attribute_group *velocitor_cdev_groups[] = {&velocitor_cdev_group,
                                                         NULL};
