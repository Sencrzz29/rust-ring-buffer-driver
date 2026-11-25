/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * User-space ABI shared by both ring-buffer drivers:
 *   /dev/rustring        (drivers/rust, safe Rust)
 *   /dev/cring           (drivers/c, C)
 *   /dev/rustring_naive, /dev/cring_naive  (deliberately unvalidated builds)
 *
 * The Rust driver re-derives the same numbers with kernel::ioctl::_IOR/_IOW;
 * the functional test suite runs every ioctl against every driver, so a
 * mismatch shows up as -ENOTTY in the tests.
 *
 * read()/write() semantics:
 *   write: stores min(count, free) bytes, returns that count;
 *          ring completely full -> -EAGAIN
 *   read:  returns min(count, used) bytes, oldest first; empty -> 0
 */
#ifndef _UAPI_RINGBUF_H
#define _UAPI_RINGBUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define RB_IOC_MAGIC 0xB5

#define RB_MIN_CAPACITY 64ULL
#define RB_MAX_CAPACITY (4ULL << 20)
#define RB_DEFAULT_CAPACITY (64ULL << 10)

struct rb_stats {
	__u64 capacity;      /* bytes the ring can hold */
	__u64 used;          /* bytes currently stored */
	__u64 bytes_written; /* total accepted by write() since load */
	__u64 bytes_read;    /* total returned by read() since load */
	__u64 rejected;      /* calls that returned an error */
};

struct rb_peek {
	__u64 offset; /* bytes past the read position */
	__u64 len;    /* bytes to copy; offset + len must be <= used */
	__u64 buf;    /* user pointer to copy into */
};

/* Fill a struct rb_stats. */
#define RB_IOC_GET_STATS _IOR(RB_IOC_MAGIC, 1, struct rb_stats)
/* Discard all stored data. */
#define RB_IOC_CLEAR     _IO(RB_IOC_MAGIC, 2)
/* Replace the buffer with one of the given capacity (__u64).
 * -EINVAL outside [RB_MIN_CAPACITY, RB_MAX_CAPACITY], -EBUSY if not empty,
 * -ENOMEM if the allocation fails. */
#define RB_IOC_RESIZE    _IOW(RB_IOC_MAGIC, 3, __u64)
/* Copy bytes without consuming them. Returns the number of bytes copied.
 * -EINVAL if offset + len overflows or exceeds `used`, -EFAULT on bad buf. */
#define RB_IOC_PEEK      _IOW(RB_IOC_MAGIC, 4, struct rb_peek)

#endif /* _UAPI_RINGBUF_H */
