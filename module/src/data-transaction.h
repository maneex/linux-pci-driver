#ifndef VELOCITOR_DATA_TRANSACTION_H
#define VELOCITOR_DATA_TRANSACTION_H

// Velocitor headers.
#include <velocitor_wire.h>

struct velocitor_dev;

// A transaction, as stored in our buffer.
struct velocitor_data_transaction {
  /// Common request header.
  struct vel_req_hdr header;

  /// Operation headers.
  union {
    /// Copy
    struct vel_copy_hdr copy;

    /// Gemm
    struct vel_gemm_hdr gemm;
  } op;

  /// Device response.
  struct vel_resp response;

  /// Position in the buffer
  int slot_idx;

  /// Engine queue carrying the request.
  u16 engine;
};

int velocitor_data_transaction_init(
    struct velocitor_dev *device, u32 engine,
    struct velocitor_data_transaction **transaction);

void velocitor_data_transaction_reset(
    struct velocitor_dev *dev, struct velocitor_data_transaction *transaction);

#endif // not VELOCITOR_DATA_TRANSACTION_H
