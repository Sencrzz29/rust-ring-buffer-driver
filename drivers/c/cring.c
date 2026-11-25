// SPDX-License-Identifier: GPL-2.0
/*
 * cring - the C counterpart of rustring.
 *
 * Same ABI (include/uapi/ringbuf.h), same algorithm (head + len ring, copies
 * straight between the ring and the user iov_iter in at most two pieces),
 * same locking (mutex for the ring because user copies may sleep, spinlock
 * for the counters) and same default capacity -- so size and speed
 * comparisons with the Rust driver are like for like.
 *
 * This file is built twice:
 *   cring.ko        careful C: every user-supplied value is validated by hand
 *   cring_naive.ko  -DCRING_NAIVE: the PEEK range check and the RESIZE bounds
 *                   and busy checks are left out -- the kind of omission that
 *                   produces real driver CVEs. Used only for the
 *                   crash-resilience experiment.
 *
 * Note what the compiler does *not* do here: nothing stops a missing check,
 * a forgotten mutex_unlock() on an error path, or a forgotten kvfree() in
 * cring_exit(). Correctness rests entirely on the programmer.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "../../include/uapi/ringbuf.h"

#ifdef CRING_NAIVE
#define DEV_NAME "cring_naive"
#else
#define DEV_NAME "cring"
#endif

struct ring {
	u8 *buf;	/* kvmalloc'd storage, NULL when not allocated */
	size_t cap;	/* bytes in buf */
	size_t head;	/* index of the oldest byte */
	size_t len;	/* bytes stored */
};

static DEFINE_MUTEX(ring_lock);
static struct ring ring;		/* protected by ring_lock */

static DEFINE_SPINLOCK(stats_lock);
static u64 stat_written, stat_read, stat_rejected; /* protected by stats_lock */

static void record(ssize_t ret, u64 *counter)
{
	spin_lock(&stats_lock);
	if (ret >= 0)
		*counter += ret;
	else
		stat_rejected++;
	spin_unlock(&stats_lock);
}

/* ------------------------------------------------------------------------ */
/* read / write                                                             */
/* ------------------------------------------------------------------------ */

static ssize_t cring_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	size_t want, first, n;
	ssize_t ret;

	mutex_lock(&ring_lock);
	if (!ring.buf) {
		ret = -ENODEV;
		goto out;
	}
	want = min(iov_iter_count(to), ring.len);
	first = min(want, ring.cap - ring.head);
	n = copy_to_iter(ring.buf + ring.head, first, to);
	if (n == first && want > first)
		n += copy_to_iter(ring.buf, want - first, to);
	if (n == 0 && want) {
		ret = -EFAULT;
		goto out;
	}
	ring.len -= n;
	ring.head = ring.len ? (ring.head + n) % ring.cap : 0;
	ret = n;
out:
	mutex_unlock(&ring_lock);
	record(ret, &stat_read);
	return ret;
}

static ssize_t cring_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	size_t want, space, tail, first, n;
	ssize_t ret;

	if (!iov_iter_count(from))
		return 0;

	mutex_lock(&ring_lock);
	if (!ring.buf) {
		ret = -ENODEV;
		goto out;
	}
	space = ring.cap - ring.len;
	if (!space) {
		ret = -EAGAIN;
		goto out;
	}
	want = min(iov_iter_count(from), space);
	tail = (ring.head + ring.len) % ring.cap;
	first = min(want, ring.cap - tail);
	n = copy_from_iter(ring.buf + tail, first, from);
	if (n == first && want > first)
		n += copy_from_iter(ring.buf, want - first, from);
	if (!n) {
		ret = -EFAULT;
		goto out;
	}
	ring.len += n;
	ret = n;
out:
	mutex_unlock(&ring_lock);
	record(ret, &stat_written);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* ioctl                                                                    */
/* ------------------------------------------------------------------------ */

static long rb_get_stats(struct rb_stats __user *arg)
{
	struct rb_stats s;

	mutex_lock(&ring_lock);
	if (!ring.buf) {
		mutex_unlock(&ring_lock);
		return -ENODEV;
	}
	s.capacity = ring.cap;
	s.used = ring.len;
	mutex_unlock(&ring_lock);

	spin_lock(&stats_lock);
	s.bytes_written = stat_written;
	s.bytes_read = stat_read;
	s.rejected = stat_rejected;
	spin_unlock(&stats_lock);

	return copy_to_user(arg, &s, sizeof(s)) ? -EFAULT : 0;
}

static long rb_clear(void)
{
	long ret = 0;

	mutex_lock(&ring_lock);
	if (ring.buf)
		ring.head = ring.len = 0;
	else
		ret = -ENODEV;
	mutex_unlock(&ring_lock);
	return ret;
}

static long rb_resize(u64 __user *arg)
{
	u64 req;
	u8 *nbuf;

	if (get_user(req, arg))
		return -EFAULT;
#ifndef CRING_NAIVE
	if (req < RB_MIN_CAPACITY || req > RB_MAX_CAPACITY)
		return -EINVAL;
#endif
	/* Allocate before taking the lock: kvmalloc may sleep. */
	nbuf = kvzalloc(req, GFP_KERNEL);
	if (!nbuf)
		return -ENOMEM;

	mutex_lock(&ring_lock);
#ifndef CRING_NAIVE
	if (ring.len) {
		mutex_unlock(&ring_lock);
		kvfree(nbuf);
		return -EBUSY;
	}
#endif
	kvfree(ring.buf);
	ring.buf = nbuf;
	ring.cap = req;
	ring.head = ring.len = 0;
	mutex_unlock(&ring_lock);
	return 0;
}

static long rb_peek(struct rb_peek __user *arg)
{
	struct rb_peek p;
	void __user *ubuf;
	size_t start;
	long ret;

	if (copy_from_user(&p, arg, sizeof(p)))
		return -EFAULT;
	ubuf = u64_to_user_ptr(p.buf);

	mutex_lock(&ring_lock);
	if (!ring.buf) {
		ret = -ENODEV;
		goto out;
	}
#ifdef CRING_NAIVE
	/*
	 * Trusts the caller: no check that [offset, offset+len) lies inside
	 * the stored data or even inside the buffer, and no wrap handling.
	 * A large len reads straight past the end of the allocation.
	 */
	start = (ring.head + p.offset) % ring.cap;
	ret = copy_to_user(ubuf, ring.buf + start, p.len) ? -EFAULT : (long)p.len;
#else
	{
		u64 end;
		size_t first;

		if (check_add_overflow(p.offset, p.len, &end) || end > ring.len) {
			ret = -EINVAL;
			goto out;
		}
		/* offset <= len <= cap and head < cap: cannot overflow */
		start = (ring.head + p.offset) % ring.cap;
		first = min_t(size_t, p.len, ring.cap - start);
		if (copy_to_user(ubuf, ring.buf + start, first) ||
		    copy_to_user(ubuf + first, ring.buf, p.len - first)) {
			ret = -EFAULT;
			goto out;
		}
		ret = p.len;
	}
#endif
out:
	mutex_unlock(&ring_lock);
	return ret;
}

static long cring_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	long ret;

	switch (cmd) {
	case RB_IOC_GET_STATS:
		ret = rb_get_stats(uarg);
		break;
	case RB_IOC_CLEAR:
		ret = rb_clear();
		break;
	case RB_IOC_RESIZE:
		ret = rb_resize(uarg);
		break;
	case RB_IOC_PEEK:
		ret = rb_peek(uarg);
		break;
	default:
		ret = -ENOTTY;
	}
	if (ret < 0) {
		spin_lock(&stats_lock);
		stat_rejected++;
		spin_unlock(&stats_lock);
	}
	return ret;
}

/* ------------------------------------------------------------------------ */
/* module                                                                   */
/* ------------------------------------------------------------------------ */

static const struct file_operations cring_fops = {
	.owner		= THIS_MODULE,
	.read_iter	= cring_read_iter,
	.write_iter	= cring_write_iter,
	.unlocked_ioctl	= cring_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice cring_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= DEV_NAME,
	.fops	= &cring_fops,
};

static int __init cring_init(void)
{
	int ret;

	ring.buf = kvzalloc(RB_DEFAULT_CAPACITY, GFP_KERNEL);
	if (!ring.buf)
		return -ENOMEM;
	ring.cap = RB_DEFAULT_CAPACITY;

	ret = misc_register(&cring_misc);
	if (ret) {
		kvfree(ring.buf);	/* every error path must undo by hand */
		ring.buf = NULL;
		return ret;
	}
	pr_info("loaded, /dev/%s capacity %zu bytes\n", DEV_NAME, ring.cap);
	return 0;
}

static void __exit cring_exit(void)
{
	misc_deregister(&cring_misc);
	pr_info("unloading, freed %zu byte buffer\n", ring.cap);
	kvfree(ring.buf);	/* forget this line and the buffer leaks */
	ring.buf = NULL;
}

module_init(cring_init);
module_exit(cring_exit);

MODULE_AUTHOR("Gaurav Gupta");
MODULE_AUTHOR("Sujeet Venkata Raman");
#ifdef CRING_NAIVE
MODULE_DESCRIPTION("Ring-buffer character device (C, deliberately unvalidated)");
#else
MODULE_DESCRIPTION("Ring-buffer character device (C)");
#endif
MODULE_LICENSE("GPL");
