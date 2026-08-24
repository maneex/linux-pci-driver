// Driver headers.
#include "handles.h"
#include "device.h"

static void velocitor_handles_free(struct kref *ref) {

  struct velocitor_handle *entry =
      container_of(ref, struct velocitor_handle, refcount);
  entry->handle = 0;
  kfree(entry);
}

static void velocitor_handles_destroy_entries(struct xarray *entries) {
  unsigned long index = 0;
  struct velocitor_handle *entry = NULL;

  xa_lock(entries);
  xa_for_each(entries, index, entry) {
    __xa_erase(entries, index);
    velocitor_handles_release(entry);
  }
  xa_unlock(entries);
}

static void velocitor_handles_destroy(void *data) {
  struct xarray *entries = data;

  velocitor_handles_destroy_entries(entries);
  xa_destroy(entries);
}

void velocitor_handles_release(struct velocitor_handle *entry) {
  if (NULL != entry)
    kref_put(&entry->refcount, velocitor_handles_free);
}

int velocitor_handles_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  xa_init(&device->handles.entries);
  return devm_add_action_or_reset(&dev->dev, velocitor_handles_destroy,
                                  &device->handles.entries);
}

void velocitor_handles_regenerate(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  unsigned long index = 0;
  struct velocitor_handle *entry = NULL;

  xa_lock(&device->handles.entries);
  if (device->handles.generation != device->rproc.generation) {
    xa_for_each(&device->handles.entries, index, entry) {
      __xa_erase(&device->handles.entries, index);
      velocitor_handles_release(entry);
    }
    device->handles.generation = device->rproc.generation;
  }
  xa_unlock(&device->handles.entries);
}

int velocitor_handles_insert(struct velocitor_dev *device, u64 size,
                             const struct velocitor_ctrl_alloc_resp *resp) {
  struct velocitor_handle *entry =
      kzalloc(sizeof(struct velocitor_handle), GFP_KERNEL);
  if (NULL == entry)
    return -ENOMEM;

  entry->size = size;
  entry->handle = le32_to_cpu(resp->handle);
  entry->node = le32_to_cpu(resp->node);
  entry->generation = le32_to_cpu(resp->generation);
  entry->dev_offset = le64_to_cpu(resp->dev_offset);
  kref_init(&entry->refcount);
  entry->created = jiffies;

  int err = 0;
  xa_lock(&device->handles.entries);
  if (le32_to_cpu(resp->generation) != device->handles.generation) {
    err = -ESTALE;
    goto error;
  }

  if ((err = __xa_insert(&device->handles.entries, entry->handle, entry,
                         GFP_ATOMIC)))
    goto error;
  xa_unlock(&device->handles.entries);

  return 0;

error:
  xa_unlock(&device->handles.entries);
  kfree(entry);
  return err;
}

int velocitor_handles_erase(struct velocitor_dev *device, u32 generation,
                            u32 handle) {
  xa_lock(&device->handles.entries);
  if (generation != device->handles.generation) {
    xa_unlock(&device->handles.entries);
    return -ESTALE;
  }
  struct velocitor_handle *it = __xa_erase(&device->handles.entries, handle);
  xa_unlock(&device->handles.entries);

  if (NULL == it)
    return -EINVAL;

  velocitor_handles_release(it);
  return 0;
}
