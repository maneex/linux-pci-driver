// Linux headers.
#include <linux/align.h>
#include <linux/overflow.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "device.h"
#include "trace.h"
#include "window.h"

static int velocitor_window_seek(struct Device *device, u32 offset) {
  writel(offset, device->bar0 + VEL_REG_WIN_BASE);
  u32 position = readl(device->bar0 + VEL_REG_WIN_BASE);
  trace_velocitor_winmove(device->last_known_winbase, position,
                          (void *)_RET_IP_);
  device->last_known_winbase = position;
  return offset == position;
}

int velocitor_window_read(struct pci_dev *dev, void *dst, size_t offset,
                          size_t size) {
  struct Device *device = pci_get_drvdata(dev);

  size_t end = 0;
  if ((!check_add_overflow(offset, size, &end)) || (end > VEL_MEM_SIZE))
    return -EINVAL;

  size_t window = ALIGN_DOWN(offset, VEL_WINDOW_SIZE);

  mutex_lock(&device->lock_winbase);

  if (!velocitor_window_seek(device, window))
    return -EIO;

  size_t done = min(size, VEL_WINDOW_SIZE + window - offset);
  memcpy_fromio(dst, device->bar2 + VEL_BAR2_WINDOW_OFF + offset - window,
                done);

  while (done < size) {
    window += VEL_WINDOW_SIZE;
    if (!velocitor_window_seek(device, window))
      return -EIO;

    size_t todo = min(size - done, VEL_WINDOW_SIZE);
    memcpy_fromio(dst + done, device->bar2 + VEL_BAR2_WINDOW_OFF, todo);
    done += todo;
  }

  mutex_unlock(&device->lock_winbase);

  return 0;
}
