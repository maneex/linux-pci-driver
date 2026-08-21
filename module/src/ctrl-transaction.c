// Linux headers.
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>

// Velocitor headers.
#include <velocitor_hw.h>

// Driver headers.
#include "ctrl-transaction.h"
#include "ctrl.h"

struct velocitor_ctrl_transaction_private {
  // Public part.
  struct velocitor_ctrl_transaction pub;

  // pointer to control
  struct velocitor_ctrl *ctrl;

  // Seq of request.
  int seq;

  // Pointer to done if sync, null if async.
  struct completion *async;

  // Pointer to callback if asnyc, null otherwise.
  void (*callback)(struct rpmsg_device *rpdev,
                   struct velocitor_ctrl_transaction *transaction);

  // Completion structure.
  struct completion done;

  // Ref counter : Once posted, we have not idea of the state of the caller. We
  //               have to keep track of the usage of the resource.
  struct kref refcount;
};

// Retrieve the private member of transaction given a pointer to the pub part of
// it.
static inline struct velocitor_ctrl_transaction_private *
to_private(struct velocitor_ctrl_transaction *transaction) {
  return container_of(transaction, struct velocitor_ctrl_transaction_private,
                      pub);
}

// Free the structure, called by kref_put when resource is unused.
static void velocitor_ctrl_transaction_free(struct kref *refcount) {
  kfree(container_of(refcount, struct velocitor_ctrl_transaction_private,
                     refcount));
}

void velocitor_ctrl_transaction_release(
    struct velocitor_ctrl_transaction *transaction) {
  kref_put(&to_private(transaction)->refcount, velocitor_ctrl_transaction_free);
}

void velocitor_ctrl_transaction_cancel(
    struct velocitor_ctrl_transaction *transaction) {
  struct velocitor_ctrl_transaction_private *private = to_private(transaction);

  mutex_lock(&private->ctrl->lock);
  idr_remove(&private->ctrl->inflight_reqs, private->seq);
  mutex_unlock(&private->ctrl->lock);

  transaction->status = -ECANCELED;
  velocitor_ctrl_transaction_notify(private->ctrl->rpdev, transaction);
  velocitor_ctrl_transaction_release(transaction);
}

// Notify a transaction to the caller
void velocitor_ctrl_transaction_notify(
    struct rpmsg_device *rpdev,
    struct velocitor_ctrl_transaction *transaction) {
  struct velocitor_ctrl_transaction_private *private = to_private(transaction);

  if (NULL != private->callback)
    private->callback(rpdev, transaction);
  if (NULL != private->async)
    complete(private->async);
}

int velocitor_ctrl_transaction_alloc(
    struct rpmsg_device *rpdev, struct velocitor_ctrl_transaction **transaction,
    void (*callback)(struct rpmsg_device *rpdev,
                     struct velocitor_ctrl_transaction *result)) {
  struct velocitor_ctrl *ctrl = dev_get_drvdata(&rpdev->dev);
  struct velocitor_ctrl_transaction_private *private = NULL;
  int seq = 0;

  if (NULL == ctrl)
    return -ENODEV;

  private = kzalloc(sizeof(*private), GFP_KERNEL);
  if (NULL == private)
    return -ENOMEM;

  private->ctrl = ctrl;
  private->callback = callback;
  init_completion(&private->done);
  if (NULL == callback)
    private->async = &private->done;
  kref_init(&private->refcount);

  mutex_lock(&ctrl->lock);
  seq = idr_alloc_cyclic(&ctrl->inflight_reqs, &private->pub, 1, 0, GFP_KERNEL);
  if (seq >= 0) {
    private->seq = seq;
    private->pub.request.msg.seq = cpu_to_le32(seq);
  }
  mutex_unlock(&ctrl->lock);

  if (seq < 0) {
    kfree(private);
    return seq;
  }

  *transaction = &private->pub;
  return 0;
}

int velocitor_ctrl_transaction_send(
    struct rpmsg_device *rpdev,
    struct velocitor_ctrl_transaction *transaction) {
  struct velocitor_ctrl_transaction_private *private = NULL;
  size_t payload_len = 0;
  int err = 0;

  if (NULL == transaction)
    return -EINVAL;

  switch (le16_to_cpu(transaction->request.msg.op)) {
  case VEL_OP_INFO:
    fallthrough;
  case VEL_OP_STAT:
    payload_len = 0;
    break;
  case VEL_OP_ALLOC:
    payload_len = sizeof(struct velocitor_ctrl_alloc_req);
    break;
  case VEL_OP_FREE:
    payload_len = sizeof(struct velocitor_ctrl_free_req);
    break;

  default:
    return -EINVAL;
  }

  private = to_private(transaction);
  kref_get(&private->refcount);

  if ((err = rpmsg_send(rpdev->ept, &transaction->request,
                        sizeof(struct velocitor_ctrl_msg) + payload_len)))
    velocitor_ctrl_transaction_cancel(transaction);

  return err;
}

int velocitor_ctrl_transaction_wait(
    struct velocitor_ctrl_transaction *transaction) {
  struct velocitor_ctrl_transaction_private *private = NULL;

  if (NULL == transaction)
    return -EINVAL;

  private = to_private(transaction);
  if ((NULL != private->callback) || (NULL == private->async))
    return -EINVAL;

  wait_for_completion(private->async);
  return transaction->status;
}
