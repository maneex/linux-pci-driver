#ifndef VELOCITOR_CTRL_TRANSACTION_H
#define VELOCITOR_CTRL_TRANSACTION_H

// Driver headers.
#include "ctrl.h"

struct velocitor_ctrl_transaction {
  // Command status.
  int status;

  // Context - opaque pointer, usage left to user.
  void *context;

  // Initial request.
  union velocitor_ctrl_request request;

  // Answer.
  union velocitor_ctrl_response answer;
};

/// Release a transaction.
/**
 *Shall be called by the caller when it does not need it anymore.
 */
void velocitor_ctrl_transaction_release(
    struct velocitor_ctrl_transaction *transaction);

/// Cancel a transaction
/**
 * Mark a transaction as canceled. Remove it from the IDR, and release it.
 * Calling this function leads the lost of ownership of the resource.
 *
 * \warn The function will still be executed by the device.
 */
void velocitor_ctrl_transaction_cancel(
    struct velocitor_ctrl_transaction *transaction);

/// Allocate the structure needed to send a message to the device.
/**
 * result will be stored in \c *transaction. It's the caller responsability to
 * properly fill the transaction->request members.
 *
 * \return 0 on success, an error number otherwise.
 */
int velocitor_ctrl_transaction_alloc(
    struct velocitor_ctrl *ctrl,
    struct velocitor_ctrl_transaction **transaction,
    void (*callback)(struct velocitor_ctrl *ctrl,
                     struct velocitor_ctrl_transaction *result));

/// Send a request to the device.
/**
 * Takes the channel under rpdev_lock for the duration of the send only, and
 * answers -ENODEV if it went away. The caller never holds a rpmsg_device.
 */
int velocitor_ctrl_transaction_send(
    struct velocitor_ctrl *ctrl,
    struct velocitor_ctrl_transaction *transaction);

/// Wait for a transaction to complete.
int velocitor_ctrl_transaction_wait(
    struct velocitor_ctrl_transaction *transaction);

/// Notify the caller that a request has been properly handled.
void velocitor_ctrl_transaction_notify(
    struct velocitor_ctrl *ctrl,
    struct velocitor_ctrl_transaction *transaction);

#endif // not VELOCITOR_CTRL_TRANSACTION_H
