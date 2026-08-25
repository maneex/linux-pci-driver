/*
 * velocitor_wire.h -- the structures both sides put on the wire.
 *
 * Sections 8.3 of the spec, shared verbatim by the QEMU model and the Linux
 * driver.  A single definition rather than one per side plus a set of
 * assertions: two declarations of the same layout can drift, and the drift is
 * only visible on the wire, which is the one place neither implementation can
 * look.
 *
 * Adjacent to velocitor_hw.h rather than inside it, on purpose.  That header
 * declares no types of its own and depends on nothing, because
 * firmware/mkfw.c consumes it as plain host C -- which is why the firmware
 * header of section 6.6 and the trace ring are given there as offsets.  These
 * structures have only two consumers, and both have fixed-width types; this
 * file may therefore ask for them.
 *
 * Endianness is not optional here (annex A.1): every field is little-endian,
 * and the kernel spelling keeps the __bitwise attribute so that sparse still
 * checks the conversions.
 */
#ifndef VELOCITOR_WIRE_H
#define VELOCITOR_WIRE_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __le16 vel_le16;
typedef __le32 vel_le32;
typedef __le64 vel_le64;
#else
#include <stdint.h>
typedef uint16_t vel_le16;
typedef uint32_t vel_le32;
typedef uint64_t vel_le64;
#endif

#include "velocitor_hw.h"

/* A range of the host's coherent pool, spec 8.3. */
struct vel_host_range {
    vel_le64 dma_addr;
    vel_le64 len;
} __attribute__((packed));

/* Heads every data plane command. */
struct vel_req_hdr {
    vel_le32 seq;
    vel_le32 generation; /* refused with -ESTALE if stale, spec 6.5 */
    vel_le16 op;         /* VEL_DATA_OP_*                           */
    vel_le16 flags;
    vel_le32 reserved;
} __attribute__((packed));

/* Follows vel_req_hdr for COPY_H2D and COPY_D2H. */
struct vel_copy_hdr {
    vel_le32 handle;
    vel_le32 reserved;
    vel_le64 offset;     /* position *within* the DeviceBuffer */
    struct vel_host_range host;
} __attribute__((packed));

/* Follows vel_req_hdr for GEMM. */
struct vel_gemm_hdr {
    vel_le32 h_a, h_b, h_c; /* 0 = not resident, read from host[] */
    vel_le32 m, n, k;
    vel_le32 dtype;
    vel_le32 flags; /* bit0 transpose A, bit1 transpose B */
    struct vel_host_range host[3];
} __attribute__((packed));

/* What the device writes back, in the chain's writable descriptor. */
struct vel_resp {
    vel_le32 seq;
    vel_le32 status;
    vel_le64 cycles;
    vel_le32 far_accesses; /* accesses to the far node, spec 3.2 */
    vel_le32 engine;       /* = the queue in v1, spec 8.3        */
} __attribute__((packed));

/*
 * Checked against the sizes velocitor_hw.h states, which is where the spec is
 * transcribed. _Static_assert rather than either side's macro: it is a C11
 * keyword and needs no header, so this file keeps depending on nothing but
 * fixed-width types.
 */
_Static_assert(sizeof(struct vel_host_range) == VEL_HOST_RANGE_SIZE, "8.3");
_Static_assert(sizeof(struct vel_req_hdr) == VEL_REQ_HDR_SIZE, "8.3");
_Static_assert(sizeof(struct vel_copy_hdr) == VEL_COPY_HDR_SIZE, "8.3");
_Static_assert(sizeof(struct vel_gemm_hdr) == VEL_GEMM_HDR_SIZE, "8.3");
_Static_assert(sizeof(struct vel_resp) == VEL_RESP_SIZE, "8.3");

#endif /* not VELOCITOR_WIRE_H */
