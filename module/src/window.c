// Linux headers.
#include <linux/align.h>
#include <linux/overflow.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "device.h"
#include "trace.h"
#include "window.h"

static ssize_t velocitor_window_debugfs_read(struct file *file,
                                             char __user *buf, size_t len,
                                             loff_t *offset) {
  struct pci_dev *dev = file->private_data;
  int res = 0;
  if (*offset >= VEL_MEM_SIZE)
    return 0;

  void *page = kmalloc(PAGE_SIZE, GFP_KERNEL);
  if (NULL == page) {
    res = -ENOMEM;
    goto out;
  }

  len = min_t(size_t, len, VEL_MEM_SIZE - *offset);
  len = min_t(size_t, len, PAGE_SIZE);

  res = velocitor_window_read(dev, page, *offset, len);
  if (0 == res) {
    if (!copy_to_user(buf, page, len))
      *offset += len;
    else
      res = -EFAULT;
  }

out:
  kfree(page);
  return res < 0 ? res : len;
}

static const struct file_operations velocitor_window_debugfs_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .read = velocitor_window_debugfs_read,
    .llseek = default_llseek,
};

static int velocitor_window_seek(struct velocitor_dev *device, u32 offset) {
  writel(offset, device->bar0 + VEL_REG_WIN_BASE);
  u32 position = readl(device->bar0 + VEL_REG_WIN_BASE);
  trace_velocitor_winmove(device->window.base, position, (void *)_RET_IP_);
  device->window.base = position;
  return offset == position;
}

int velocitor_window_read(struct pci_dev *dev, void *dst, size_t offset,
                          size_t size) {
  struct velocitor_dev *device = pci_get_drvdata(dev);
  int err = 0;
  size_t end = 0;
  if ((check_add_overflow(offset, size, &end)) || (end > VEL_MEM_SIZE))
    return -EINVAL;

  size_t window = ALIGN_DOWN(offset, VEL_WINDOW_SIZE);

  mutex_lock(&device->window.lock);

  if (!velocitor_window_seek(device, window)) {
    err = -EIO;
    goto out;
  }

  size_t done = min(size, VEL_WINDOW_SIZE + window - offset);
  memcpy_fromio(dst, device->bar2 + VEL_BAR2_WINDOW_OFF + offset - window,
                done);

  while (done < size) {
    window += VEL_WINDOW_SIZE;
    if (!velocitor_window_seek(device, window)) {
      err = -EIO;
      goto out;
    }

    size_t todo = min(size - done, VEL_WINDOW_SIZE);
    memcpy_fromio(dst + done, device->bar2 + VEL_BAR2_WINDOW_OFF, todo);
    done += todo;
  }

out:
  mutex_unlock(&device->window.lock);
  return err;
}

int velocitor_window_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  int err = 0;
  if ((err = devm_mutex_init(&dev->dev, &device->window.lock)))
    return err;

  debugfs_create_file_size("mem", 0400, device->debugfs, dev,
                           &velocitor_window_debugfs_fops, VEL_MEM_SIZE);
  return 0;
}
