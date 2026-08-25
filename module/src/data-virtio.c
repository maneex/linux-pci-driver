// Linux headers.
#include <linux/remoteproc.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

// Driver headers.
#include "data-transaction.h"
#include "data-virtio.h"
#include "data.h"
#include "device.h"

static void velocitor_data_virtio_callback(struct virtqueue *vq) {
  struct velocitor_dev *device = vq->vdev->priv;
  struct velocitor_data_transaction *transaction;
  unsigned int len;

  while ((transaction = virtqueue_get_buf(vq, &len)) != NULL) {
    velocitor_data_complete(device, transaction, len);
    velocitor_data_transaction_reset(device, transaction);
  }
}

static struct velocitor_dev *
velocitor_data_virtio_device(struct virtio_device *vdev) {
  struct rproc *rproc = rproc_get_by_child(&vdev->dev);

  if (NULL == rproc || NULL == rproc->dev.parent)
    return NULL;

  return pci_get_drvdata(to_pci_dev(rproc->dev.parent));
}

static int velocitor_data_virtio_probe(struct virtio_device *vdev) {
  static struct virtqueue_info vqs_info[VEL_ENGINES] = {
      {.name = "engineq0", .callback = velocitor_data_virtio_callback},
      {.name = "engineq1", .callback = velocitor_data_virtio_callback}};

  dev_info(&vdev->dev, "virtio: creating engine queues");

  struct velocitor_dev *device = velocitor_data_virtio_device(vdev);
  if (NULL == device)
    return -ENODEV;
  vdev->priv = device;

  int err = 0;
  if ((err = virtio_find_vqs(vdev, VEL_ENGINES, device->data.vqs, vqs_info,
                             NULL)))
    return err;
  return 0;
}

static void velocitor_data_virtio_remove(struct virtio_device *vdev) {
  struct velocitor_dev *device = (struct velocitor_dev *)vdev->priv;

  virtio_reset_device(vdev);

  struct velocitor_data_transaction *transaction = NULL;
  for (unsigned int i = 0; i < VEL_ENGINES; ++i)
    while ((transaction = virtqueue_detach_unused_buf(device->data.vqs[i])) !=
           NULL) {
      transaction->response.status = cpu_to_le32(-ESTALE);
      velocitor_data_complete(device, transaction, 0);
      velocitor_data_transaction_reset(device, transaction);
    }

  vdev->config->del_vqs(vdev);
  vdev->priv = NULL;
}

static const struct virtio_device_id velocitor_data_virtio_id_table[] = {
    {.device = VEL_VIRTIO_ID, .vendor = VIRTIO_DEV_ANY_ID}, {}};
MODULE_DEVICE_TABLE(virtio, velocitor_data_virtio_id_table);

static struct virtio_driver velocitor_data_virtio_driver = {
    .driver.name = "velocitor-data",
    .id_table = velocitor_data_virtio_id_table,
    .feature_table = NULL,
    .feature_table_size = 0,
    .probe = velocitor_data_virtio_probe,
    .remove = velocitor_data_virtio_remove};

int velocitor_data_virtio_initialize(void) {
  return register_virtio_driver(&velocitor_data_virtio_driver);
}

void velocitor_data_virtio_release(void) {
  unregister_virtio_driver(&velocitor_data_virtio_driver);
}
