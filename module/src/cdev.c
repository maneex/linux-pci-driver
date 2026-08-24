// Linux headers.
#include <linux/build_bug.h>
#include <linux/fs.h>

// Driver headers.
#include "cdev-sysfs.h"
#include "cdev.h"
#include "counters.h"
#include "ctrl-transaction.h"
#include "device.h"
#include "handles.h"

// https://www.kernel.org/doc/html/v5.6/driver-api/infrastructure.html
// https://linux-kernel-labs.github.io/refs/heads/master/labs/device_model.html?highlight=device_create
// https://docs.kernel.org/dev-tools/checkuapi.html

/*
 * The UAPI shapes its structures for VEL_UAPI_NODES, which is frozen; the
 * hardware has VEL_NODES, which is not. They have to agree, and this is where
 * that is checked -- once, at build time. The day the model gains a node this
 * stops the build, which is the point: the alternative is an ABI that changes
 * size under already-compiled binaries without a line of uapi/ being touched.
 */
static_assert(VEL_UAPI_NODES == VEL_NODES,
              "the UAPI node count no longer matches the hardware");

static void velocitor_cdev_unregister_region(void *dn) {
  dev_t *devno = dn;
  unregister_chrdev_region(*devno, 1);
}

static void velocitor_cdev_destroy_device(void *d) {
  struct velocitor_dev *device = d;
  device_destroy(device->cdev.class, device->cdev.devno);
}

static void velocitor_cdev_del(void *c) {
  struct cdev *cdev = c;
  cdev_del(cdev);
}

static int velocitor_cdev_open(struct inode *inode, struct file *file) {
  struct velocitor_cdev *cdev =
      container_of(inode->i_cdev, struct velocitor_cdev, cdev);
  struct velocitor_dev *device = container_of(cdev, struct velocitor_dev, cdev);

  if (test_and_set_bit(0, &device->cdev.usageFlags))
    return -EBUSY;

  file->private_data = device;
  return 0;
}

static int velocitor_cdev_release(struct inode *inode, struct file *file) {
  struct velocitor_dev *device = file->private_data;

  clear_bit(0, &device->cdev.usageFlags);
  return 0;
}

static int velocitor_safe_copy_to_user(void __user *udst, size_t usize,
                                       void *ksrc, size_t ksize) {
  bool ignored_trailing = false;
  int err = copy_struct_to_user(udst, usize, ksrc, ksize, &ignored_trailing);
  if (err)
    return err;
  return ignored_trailing ? -EMSGSIZE : 0;
}

static long int velocitor_cdev_ioctl_info(struct velocitor_dev *device,
                                          size_t usize, void __user *argp) {
  struct velocitor_ioctl_info info = {};
  info.generation = device->rproc.generation;
  switch (readl(device->bar0 + VEL_REG_FW_STATUS)) {
  case VEL_FW_STATUS_RESET:
    info.state = VEL_DEVICE_INFO_DOWN;
    break;

  case VEL_FW_STATUS_VERIFIED:
    info.state = VEL_DEVICE_INFO_BOOTING;
    break;

  case VEL_FW_STATUS_RUNNING:
    info.state = VEL_DEVICE_INFO_READY;
    break;

  default:
    info.state = VEL_DEVICE_INFO_CRASHED;
    break;
  }

  return velocitor_safe_copy_to_user(argp, usize, &info,
                                     sizeof(struct velocitor_ioctl_info));
}

static long int velocitor_cdev_ioctl_stats(struct velocitor_dev *device,
                                           size_t usize, void __user *argp) {
  int err = 0;
  struct velocitor_ctrl_transaction *transaction = NULL;
  if ((err =
           velocitor_ctrl_transaction_alloc(&device->ctrl, &transaction, NULL)))
    return err;

  transaction->request.msg.op = cpu_to_le16(VEL_OP_STAT);
  if ((err = velocitor_ctrl_transaction_send(&device->ctrl, transaction)))
    goto out;

  if ((err = velocitor_ctrl_transaction_wait(transaction)))
    goto out;

  struct velocitor_ioctl_stats stats = {};
  stats.live_handles = le32_to_cpu(transaction->answer.stat.live_handles);
  for (unsigned int i = 0; i < VEL_NODES; ++i) {
    stats.nodes[i].capacity =
        le64_to_cpu(transaction->answer.stat.node[i].capacity);
    stats.nodes[i].free = le64_to_cpu(transaction->answer.stat.node[i].free);
  }
  err = velocitor_safe_copy_to_user(argp, usize, &stats,
                                    sizeof(struct velocitor_ioctl_stats));

out:
  velocitor_ctrl_transaction_release(transaction);
  return err;
}

static long int velocitor_cdev_ioctl_alloc(struct velocitor_dev *device,
                                           size_t usize, void __user *argp) {

  int err = 0;
  struct velocitor_ioctl_alloc alloc = {};
  if ((err = copy_struct_from_user(&alloc, sizeof(struct velocitor_ioctl_alloc),
                                   argp, usize)))
    return err;

  struct velocitor_ctrl_transaction *transaction = NULL;
  if ((err =
           velocitor_ctrl_transaction_alloc(&device->ctrl, &transaction, NULL)))
    return err;

  transaction->request.msg.op = cpu_to_le16(VEL_OP_ALLOC);

  struct velocitor_ctrl_alloc_req *req =
      (void *)transaction->request.msg.payload;
  req->size = cpu_to_le64(alloc.size);
  req->dtype = cpu_to_le32(alloc.dtype);
  req->node = cpu_to_le32(alloc.node);
  if ((err = velocitor_ctrl_transaction_send(&device->ctrl, transaction)))
    goto out;

  if ((err = velocitor_ctrl_transaction_wait(transaction)))
    goto out;

  // FIXME ! We shall post a free if we can't insert or copy_to_user fails.
  if ((err = velocitor_handles_insert(device, alloc.size,
                                      &transaction->answer.alloc)))
    goto out;

  alloc.handle.addr = le32_to_cpu(transaction->answer.alloc.handle);
  alloc.handle.node = le32_to_cpu(transaction->answer.alloc.node);
  alloc.handle.generation = le32_to_cpu(transaction->answer.alloc.generation);

  err = velocitor_safe_copy_to_user(argp, usize, &alloc,
                                    sizeof(struct velocitor_ioctl_alloc));

out:
  velocitor_ctrl_transaction_release(transaction);
  return err;
}

static long int velocitor_cdev_ioctl_free(struct velocitor_dev *device,
                                          size_t usize, void __user *argp) {
  int err = 0;
  struct velocitor_ioctl_handle handle = {};
  if ((err = copy_struct_from_user(
           &handle, sizeof(struct velocitor_ioctl_handle), argp, usize)))
    return err;

  if ((err = velocitor_handles_erase(device, handle.generation, handle.addr)))
    return err;

  struct velocitor_ctrl_transaction *transaction = NULL;
  if ((err =
           velocitor_ctrl_transaction_alloc(&device->ctrl, &transaction, NULL)))
    return err;

  transaction->request.msg.op = cpu_to_le16(VEL_OP_FREE);
  struct velocitor_ctrl_free_req *req =
      (void *)transaction->request.msg.payload;
  req->handle = cpu_to_le32(handle.addr);

  if ((err = velocitor_ctrl_transaction_send(&device->ctrl, transaction)))
    goto out;

  err = velocitor_ctrl_transaction_wait(transaction);

out:
  velocitor_ctrl_transaction_release(transaction);
  return err;
}

static long int velocitor_cdev_ioctl(struct file *file, unsigned int cmd,
                                     unsigned long argp) {
  if (_IOC_TYPE(cmd) != VEL_DRV_MAGIC)
    return -ENOTTY;

  switch (_IOC_NR(cmd)) {
  case _IOC_NR(VEL_IOC_INFO):
    return velocitor_cdev_ioctl_info(file->private_data, _IOC_SIZE(cmd),
                                     (void __user *)argp);
  case _IOC_NR(VEL_IOC_ALLOC_DEV):
    return velocitor_cdev_ioctl_alloc(file->private_data, _IOC_SIZE(cmd),
                                      (void __user *)argp);
  case _IOC_NR(VEL_IOC_FREE_DEV):
    return velocitor_cdev_ioctl_free(file->private_data, _IOC_SIZE(cmd),
                                     (void __user *)argp);
  case _IOC_NR(VEL_IOC_STATS):
    return velocitor_cdev_ioctl_stats(file->private_data, _IOC_SIZE(cmd),
                                      (void __user *)argp);
  default:
    return -ENOTTY;
  }
  return 0;
}

static const struct file_operations velocitor_cdev_ops = {
    .open = velocitor_cdev_open,
    .release = velocitor_cdev_release,
    .unlocked_ioctl = velocitor_cdev_ioctl};

int velocitor_cdev_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);

  int err = 0;
  if ((err = alloc_chrdev_region(&device->cdev.devno, 0, 1, KBUILD_MODNAME)))
    return err;
  if ((err = devm_add_action_or_reset(
           &dev->dev, velocitor_cdev_unregister_region, &device->cdev.devno)))
    return err;

  cdev_init(&device->cdev.cdev, &velocitor_cdev_ops);
  device->cdev.cdev.owner = THIS_MODULE;

  if ((err = cdev_add(&device->cdev.cdev, device->cdev.devno, 1)))
    return err;
  if ((err = devm_add_action_or_reset(&dev->dev, velocitor_cdev_del,
                                      &device->cdev.cdev)))
    return err;

  device->cdev.device = device_create_with_groups(
      device->cdev.class, &dev->dev, device->cdev.devno, device,
      velocitor_cdev_groups, KBUILD_MODNAME "0");
  if (IS_ERR(device->cdev.device))
    return PTR_ERR(device->cdev.device);
  if ((err = devm_add_action_or_reset(&dev->dev, velocitor_cdev_destroy_device,
                                      device)))
    return err;

  return 0;
}
