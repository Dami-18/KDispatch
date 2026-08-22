/* SPDX-License-Identifier: GPL-2.0 */
/* Shared between the kdispatch kernel module and its userspace server. */
#ifndef _KDISPATCH_UAPI_H
#define _KDISPATCH_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

#define KD_DEVICE_NAME "kdispatch"
#define KD_DEVICE_PATH "/dev/" KD_DEVICE_NAME

/* Wire constraints, mirrored from include/proto.hpp. */
#define KD_MIN_MSG 32
#define KD_MAX_MSG (8u * 1024 * 1024)

struct kd_attach {
	__s32 fd;        /* TCP socket to take over the receive path of */
	__u32 _pad;
};

struct kd_stats {
	__u64 msgs;      /* messages delivered to userspace */
	__u64 bytes;
	__u32 qlen;      /* messages queued right now */
	__u32 qmax;      /* high-water mark */
	__u32 conns;     /* attached transports */
	__u32 pauses;    /* times backpressure paused a parser */
	__u32 aborts;    /* parser aborts (protocol errors, resets) */
	__u32 desync;    /* messages whose length prefix disagreed with full_len */
};

#define KD_IOC_MAGIC 'K'
#define KD_ATTACH _IOW(KD_IOC_MAGIC, 1, struct kd_attach)
#define KD_STATS  _IOR(KD_IOC_MAGIC, 2, struct kd_stats)
/* Wake every blocked reader and make further reads return 0, so workers can
 * exit without depending on signal-interruption semantics. */
#define KD_SHUTDOWN _IO(KD_IOC_MAGIC, 3)

#endif /* _KDISPATCH_UAPI_H */
