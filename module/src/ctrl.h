#ifndef VELOCITOR_CTRL_H
#define VELOCITOR_CTRL_H

// Linux headers.
#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rpmsg.h>
#include <linux/rwsem.h>
#include <linux/stddef.h>
#include <linux/types.h>

// Velocitor headers.
#include <velocitor_hw.h>

struct velocitor_ctrl_msg {
  __le32 seq;
  __le16 op;
  __le16 flags;
  __le32 status;   /* réponse uniquement, 0 = OK, négatif = -errno */
  __le32 reserved; /* écrit à zéro, ignoré en lecture */
  __u8 payload[];
};

struct velocitor_ctrl_info_resp {
  __le32 abi;       /* VEL_FW_ABI                              */
  __le32 caps;      /* ce que le firmware croit du matériel    */
  __le32 ctrl_caps; /* bit0 : respecte le nœud demandé         */
  __le32 nodes, engines;
  __le32 alloc_align;
  __le32 generation;
};

struct velocitor_ctrl_alloc_req {
  __le64 size;
  __le32 dtype;
  __le32 node;
};

struct velocitor_ctrl_alloc_resp {
  __le32 handle;
  __le32 node;
  __le32 generation;
  __le32 reserved;
  __le64 dev_offset;
};

struct velocitor_ctrl_free_req {
  __le32 handle;
  __le32 reserved;
};

struct velocitor_ctrl_stat_node {
  __le64 capacity;
  __le64 free;
};

struct velocitor_ctrl_stat_resp {
  struct velocitor_ctrl_stat_node node[VEL_NODES];
  __le32 live_handles;
  __le32 reserved;
};

union velocitor_ctrl_req_payload {
  struct velocitor_ctrl_alloc_req alloc;
  struct velocitor_ctrl_free_req free;
};

union velocitor_ctrl_request {
  struct velocitor_ctrl_msg msg;
  union velocitor_ctrl_req_payload align_;
  __u8 bytes[sizeof(struct velocitor_ctrl_msg) +
             sizeof(union velocitor_ctrl_req_payload)];
};

union velocitor_ctrl_response {
  struct velocitor_ctrl_info_resp info;
  struct velocitor_ctrl_alloc_resp alloc;
  struct velocitor_ctrl_stat_resp stat;
};

struct velocitor_ctrl_transaction;

struct velocitor_ctrl {
  // The channel, while there is one. NULL between two generations.
  struct rw_semaphore rpdev_lock;
  struct rpmsg_device *rpdev;

  struct device *dev;

  struct mutex lock;
  struct idr inflight_reqs;

  // The INFO handshake outlives probe(), so it cannot live on its stack.
  struct velocitor_ctrl_transaction *info_transaction;
  struct velocitor_ctrl_info_resp info;
  bool info_valid; /* info holds an answer, not zeroes */
};

int velocitor_ctrl_initialize(struct pci_dev *dev);

int velocitor_ctrl_rpmsg_initialize(void);
void velocitor_ctrl_rpmsg_release(void);

#endif // not VELOCITOR_CTRL_H
