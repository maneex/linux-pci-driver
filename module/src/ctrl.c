// Linux headers.
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "ctrl-transaction.h"
#include "ctrl.h"
#include "device.h"

// https://docs.kernel.org/staging/rpmsg.html

// https://www.kernel.org/doc/Documentation/scheduler/completion.txt
// https://docs.kernel.org/core-api/idr.html
#define VELOCITOR_RPMSG_TIMEOUT_MS 500

static void velocitor_ctrl_destroy(void *inflight_reqs) {
  idr_destroy(inflight_reqs);
}

static struct velocitor_dev *velocitor_ctrl_device(struct rpmsg_device *rpdev) {
  struct rproc *rproc = rproc_get_by_child(&rpdev->dev);

  if (NULL == rproc || NULL == rproc->dev.parent)
    return NULL;

  return pci_get_drvdata(to_pci_dev(rproc->dev.parent));
}

static void
velocitor_ctrl_oninfo(struct rpmsg_device *rpdev,
                      struct velocitor_ctrl_transaction *transaction) {
  struct velocitor_ctrl *ctrl = dev_get_drvdata(&rpdev->dev);

  if (transaction->status) {
    dev_err(&rpdev->dev, "rpmsg: INFO answered %d", transaction->status);
    return;
  }
  ctrl->info = transaction->answer.info;
  ctrl->info_valid = true;
  dev_info(&rpdev->dev,
           "rpmsg: firmware abi %u, caps 0x%08x, ctrl_caps 0x%08x, %u node(s), "
           "%u engine(s), align %u, generation %u",
           le32_to_cpu(ctrl->info.abi), le32_to_cpu(ctrl->info.caps),
           le32_to_cpu(ctrl->info.ctrl_caps), le32_to_cpu(ctrl->info.nodes),
           le32_to_cpu(ctrl->info.engines), le32_to_cpu(ctrl->info.alloc_align),
           le32_to_cpu(ctrl->info.generation));
}

static int velocitor_ctrl_rpmsg_probe(struct rpmsg_device *rpdev) {
  struct velocitor_ctrl_transaction *transaction = NULL;
  struct velocitor_dev *device = NULL;
  struct velocitor_ctrl *ctrl = NULL;
  int err = 0;

  dev_info(&rpdev->dev, "rpmsg: creating channel: 0x%x -> 0x%x", rpdev->src,
           rpdev->dst);

  device = velocitor_ctrl_device(rpdev);
  if (NULL == device)
    return -ENODEV;

  // The table belongs to the card and survives this channel; only the binding
  // to the channel is set up here. drvdata stays the shortcut every reply
  // takes, so it caches what the walk above found.
  ctrl = &device->ctrl;
  ctrl->rpdev = rpdev;
  dev_set_drvdata(&rpdev->dev, ctrl);

  if ((err = velocitor_ctrl_transaction_alloc(rpdev, &transaction,
                                              velocitor_ctrl_oninfo)))
    goto error;

  transaction->request.msg.op = cpu_to_le16(VEL_OP_INFO);

  err = velocitor_ctrl_transaction_send(rpdev, transaction);
  velocitor_ctrl_transaction_release(transaction);
  if (err) {
    dev_err(&rpdev->dev, "rpmsg: INFO failed: %d", err);
    goto error;
  }

  return 0;

error:
  ctrl->rpdev = NULL;
  dev_set_drvdata(&rpdev->dev, NULL);
  return err;
}

static void velocitor_ctrl_rpmsg_remove(struct rpmsg_device *rpdev) {
  struct velocitor_ctrl *ctrl = dev_get_drvdata(&rpdev->dev);

  dev_info(&rpdev->dev, "rpmsg: destroying channel");

  // Destroy the entry point, to get sure that we don't get more replies while
  // destroying.
  rpmsg_destroy_ept(rpdev->ept);
  rpdev->ept = NULL;

  // Mark in flight transactions staled, notify callers.
  while (true) {
    int seq = 0;

    mutex_lock(&ctrl->lock);
    struct velocitor_ctrl_transaction *transaction =
        idr_get_next(&ctrl->inflight_reqs, &seq);
    if (NULL == transaction) {
      mutex_unlock(&ctrl->lock);
      break;
    }

    idr_remove(&ctrl->inflight_reqs, seq);
    mutex_unlock(&ctrl->lock);

    transaction->status = -ESTALE;
    velocitor_ctrl_transaction_notify(rpdev, transaction);
    velocitor_ctrl_transaction_release(transaction);
  }

  // The table stays, the binding goes: the card keeps both across generations.
  ctrl->rpdev = NULL;
  dev_set_drvdata(&rpdev->dev, NULL);
}

static int velocitor_ctrl_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
                                         int len, void *priv, u32 src) {
  // Sanity check. minimum size of a callback is the size of message
  if (len < (int)sizeof(struct velocitor_ctrl_msg)) {
    dev_err(&rpdev->dev, "rpmsg: truncated message of %d byte(s)", len);
    return 0;
  }

  const struct velocitor_ctrl_msg *msg = data;
  struct velocitor_ctrl *ctrl = dev_get_drvdata(&rpdev->dev);
  if (NULL == ctrl)
    return 0;

  // Find the transaction in the queue.
  mutex_lock(&ctrl->lock);
  struct velocitor_ctrl_transaction *transaction =
      idr_find(&ctrl->inflight_reqs, le32_to_cpu(msg->seq));
  if (NULL == transaction) {
    mutex_unlock(&ctrl->lock);
    dev_warn(&rpdev->dev, "rpmsg: no waiter for seq %u, dropped",
             le32_to_cpu(msg->seq));
    return 0;
  }

  // Remove transaction from inflight queue
  idr_remove(&ctrl->inflight_reqs, le32_to_cpu(msg->seq));
  mutex_unlock(&ctrl->lock);

  // Sanity checks.
  if (msg->op != transaction->request.msg.op) {
    dev_warn(&rpdev->dev, "rpmsg: seq %u answers op %u, asked op %u",
             le32_to_cpu(msg->seq), le16_to_cpu(msg->op),
             le16_to_cpu(transaction->request.msg.op));
    transaction->status = -EREMOTEIO;
    goto out;
  }

  s32 status = (s32)le32_to_cpu(msg->status);
  transaction->status =
      ((status <= 0) && (status >= -MAX_ERRNO)) ? status : -EREMOTEIO;
  if (transaction->status)
    goto out;

  size_t answer_len = 0;
  size_t payload_len = len - sizeof(*msg);
  switch (le16_to_cpu(transaction->request.msg.op)) {
  case VEL_OP_INFO:
    answer_len = sizeof(struct velocitor_ctrl_info_resp);
    break;
  case VEL_OP_ALLOC:
    answer_len = sizeof(struct velocitor_ctrl_alloc_resp);
    break;
  case VEL_OP_FREE:
    answer_len = 0;
    break;
  case VEL_OP_STAT:
    answer_len = sizeof(struct velocitor_ctrl_stat_resp);
    break;
  default:
    transaction->status = -EREMOTEIO;
    goto out;
  }

  if (payload_len < answer_len) {
    dev_warn(&rpdev->dev,
             "rpmsg: seq %u op %u, invalid request size %zu, awaited %zu",
             le32_to_cpu(msg->seq), le16_to_cpu(msg->op), payload_len,
             answer_len);
    transaction->status = -EPROTO;
    goto out;
  }

  memcpy(&transaction->answer, msg->payload, answer_len);

out:
  // Notify completion, callback if needed.
  velocitor_ctrl_transaction_notify(rpdev, transaction);
  velocitor_ctrl_transaction_release(transaction);
  return 0;
}

int velocitor_ctrl_initialize(struct pci_dev *dev) {
  struct velocitor_dev *device = pci_get_drvdata(dev);
  int err = 0;

  if ((err = devm_mutex_init(&dev->dev, &device->ctrl.lock)))
    return err;

  idr_init(&device->ctrl.inflight_reqs);
  return devm_add_action_or_reset(&dev->dev, velocitor_ctrl_destroy,
                                  &device->ctrl.inflight_reqs);
}

static const struct rpmsg_device_id velocitor_ctrl_rpmsg_id_table[] = {
    {.name = VEL_CTRL_NAME}, {}};
MODULE_DEVICE_TABLE(rpmsg, velocitor_ctrl_rpmsg_id_table);

static struct rpmsg_driver velocitor_ctrl_rpmsg_driver = {
    .drv.name = "velocitor-ctrl",
    .id_table = velocitor_ctrl_rpmsg_id_table,
    .probe = velocitor_ctrl_rpmsg_probe,
    .callback = velocitor_ctrl_rpmsg_callback,
    .remove = velocitor_ctrl_rpmsg_remove};

int velocitor_ctrl_rpmsg_initialize(void) {
  return register_rpmsg_driver(&velocitor_ctrl_rpmsg_driver);
}

void velocitor_ctrl_rpmsg_release(void) {
  unregister_rpmsg_driver(&velocitor_ctrl_rpmsg_driver);
}
