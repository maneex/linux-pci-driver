// Linux headers.
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "device-trace.h"
#include "device.h"

struct velocitor_dtrace_header {
  struct pci_dev *dev;
  u32 skipped;
  u32 dropped;
  u32 tail;
  u32 head;
  u32 count;
} __aligned(32);

struct velocitor_dtrace_entry {
  __le64 timestamp;
  __le16 level;
  __le16 engine;
  __le32 seq;
  char msg[112];
};
static_assert(sizeof(struct velocitor_dtrace_entry) == VEL_TRACE_ENTRY);

#define VEL_TRACE_ENGINE_NONE 0xFFFFu

static int velocitor_dtrace_debugfs_show(struct seq_file *m, void *unused) {
  const struct velocitor_dtrace_header *header = m->private;
  const struct velocitor_dtrace_entry *entries = (const void *)(header + 1);

  seq_printf(m, "cursor   %u\n", header->tail);
  seq_printf(m, "head     %u\n", header->head);
  seq_printf(m, "entries  %u\n", header->count);
  seq_printf(m, "skipped  %u\n", header->skipped);
  seq_printf(m, "dropped  %u\n\n", header->dropped);

  for (u32 idx = 0; idx < header->count; ++idx) {
    const struct velocitor_dtrace_entry *entry = &entries[idx];
    u16 engine = le16_to_cpu(entry->engine);

    seq_printf(m, "%u %llu ", header->tail + header->skipped + idx,
               le64_to_cpu(entry->timestamp));

    if (VEL_TRACE_ENGINE_NONE == engine)
      seq_puts(m, "e- ");
    else
      seq_printf(m, "e%u ", engine);

    seq_printf(m, "l%u s%u %.*s\n", le16_to_cpu(entry->level),
               le32_to_cpu(entry->seq),
               (int)strnlen(entry->msg, sizeof(entry->msg)), entry->msg);
  }

  return 0;
}

static int velocitor_dtrace_debugfs_release(struct inode *inode,
                                            struct file *file) {
  struct seq_file *sf = file->private_data;
  kvfree(sf->private);

  return single_release(inode, file);
}

static int velocitor_dtrace_debugfs_open(struct inode *inode,
                                         struct file *file) {
  int result = 0;
  struct velocitor_dev *device =
      pci_get_drvdata((struct pci_dev *)inode->i_private);
  u32 head = 0;
  u32 tail = 0;
  u32 dropped = 0;

  mutex_lock(&device->dtrace.lock);
  if (0 == device->dtrace.da) {
    result = -ENODEV;
    goto out;
  }

  head = readl(device->bar2 + device->dtrace.da + VEL_TRACE_OFF_HEAD);
  tail = readl(device->bar2 + device->dtrace.da + VEL_TRACE_OFF_TAIL);
  dropped = readl(device->bar2 + device->dtrace.da + VEL_TRACE_OFF_DROPPED);

  u32 entries = min(head - tail, VEL_TRACE_ENTRIES);
  void *private_data = kvmalloc(sizeof(struct velocitor_dtrace_header) +
                                    entries * VEL_TRACE_ENTRY,
                                GFP_KERNEL);
  if (NULL == private_data) {
    result = -ENOMEM;
    goto out;
  }

  struct velocitor_dtrace_header *header = private_data;
  header->dev = inode->i_private;
  header->head = head;
  header->tail = tail;
  header->dropped = dropped;
  header->count = entries;

  if (head - tail > VEL_TRACE_ENTRIES) {
    header->skipped = head - tail - VEL_TRACE_ENTRIES;
    tail = head - VEL_TRACE_ENTRIES;
  } else
    header->skipped = 0;

  result = single_open(file, velocitor_dtrace_debugfs_show, private_data);
  if (result < 0) {
    kvfree(private_data);
    goto out;
  }

  void *entry = header + 1;
  for (u32 idx = 0; idx < entries; ++idx)
    memcpy_fromio(entry + idx * VEL_TRACE_ENTRY,
                  device->bar2 + device->dtrace.da + VEL_TRACE_HDR_SIZE +
                      ((tail + idx) % VEL_TRACE_ENTRIES) * VEL_TRACE_ENTRY,
                  VEL_TRACE_ENTRY);

  writel(head, device->bar2 + device->dtrace.da + VEL_TRACE_OFF_TAIL);

out:
  mutex_unlock(&device->dtrace.lock);
  return result;
}

static const struct file_operations velocitor_dtrace_debugfs_fops = {
    .owner = THIS_MODULE,
    .open = velocitor_dtrace_debugfs_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = velocitor_dtrace_debugfs_release};

int velocitor_dtrace_initialize(struct pci_dev *dev) {
  int err = 0;
  struct velocitor_dev *device = pci_get_drvdata(dev);

  if ((err = devm_mutex_init(&dev->dev, &device->dtrace.lock)))
    return err;

  // Named by spec section 11.  0400: the ring is the firmware's log, and
  // reading it consumes it -- root only, and no writer.
  debugfs_create_file("trace", 0400, device->debugfs, dev,
                      &velocitor_dtrace_debugfs_fops);

  return 0;
}
