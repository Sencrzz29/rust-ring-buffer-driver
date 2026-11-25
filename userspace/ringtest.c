// SPDX-License-Identifier: GPL-2.0
/*
 * ringtest - functional, adversarial and concurrency tests for a
 * ring-buffer device (rustring or cring; same ABI).
 *
 *   ringtest <dev> [--quick] [--only <group>]
 *
 * Groups: basic wrap peek hostile badptr ioctl spsc mpmc race
 *
 * Output is TAP-like ("ok N - ...", "not ok N - ..."), ending with
 *   # ringtest <dev>: PASS n/n     or     # ringtest <dev>: FAIL f/n
 * Exit status 0 only if every check passed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <uapi/ringbuf.h>

#define KERNEL_ADDR ((void *)0xffff000000000000ULL)
/* volatile: keep the compiler from rejecting deliberate NULL arguments */
static void *volatile null_ptr;

static const char *dev;
static bool quick;
static int ntests, nfail;

static void check(bool cond, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(bool cond, const char *fmt, ...)
{
	va_list ap;

	ntests++;
	if (!cond)
		nfail++;
	printf("%s %d - ", cond ? "ok" : "not ok", ntests);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

/* ---- thin wrappers returning -errno ------------------------------------ */

static int xopen(void)
{
	int fd = open(dev, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		exit(2);
	}
	return fd;
}

static long xioctl(int fd, unsigned long cmd, void *arg)
{
	long r = ioctl(fd, cmd, arg);

	return r < 0 ? -errno : r;
}

static long xwrite(int fd, const void *b, size_t n)
{
	ssize_t r = write(fd, b, n);

	return r < 0 ? -errno : r;
}

static long xread(int fd, void *b, size_t n)
{
	ssize_t r = read(fd, b, n);

	return r < 0 ? -errno : r;
}

static long resize(int fd, uint64_t cap)
{
	return xioctl(fd, RB_IOC_RESIZE, &cap);
}

static long peek(int fd, uint64_t off, uint64_t len, void *buf)
{
	struct rb_peek p = { .offset = off, .len = len, .buf = (uintptr_t)buf };

	return xioctl(fd, RB_IOC_PEEK, &p);
}

static struct rb_stats stats(int fd)
{
	struct rb_stats s;

	memset(&s, 0, sizeof(s));
	if (xioctl(fd, RB_IOC_GET_STATS, &s)) {
		fprintf(stderr, "GET_STATS failed\n");
		exit(2);
	}
	return s;
}

static void reset(int fd, uint64_t cap)
{
	long r;

	xioctl(fd, RB_IOC_CLEAR, NULL);
	r = resize(fd, cap);
	if (r) {
		fprintf(stderr, "reset to %" PRIu64 " failed: %ld\n", cap, r);
		exit(2);
	}
}

static inline uint8_t pat(uint64_t i)
{
	return (uint8_t)(i % 251);
}

static uint64_t rnd(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- groups ------------------------------------------------------------ */

static void t_basic(int fd)
{
	uint8_t in[100], out[200];
	struct rb_stats s;
	long r;

	reset(fd, 4096);
	for (int i = 0; i < 100; i++)
		in[i] = pat(i);
	r = xwrite(fd, in, sizeof(in));
	check(r == 100, "basic: write 100 bytes returns 100 (got %ld)", r);
	r = xread(fd, out, sizeof(out));
	check(r == 100 && !memcmp(in, out, 100), "basic: read returns the same 100 bytes (got %ld)", r);
	r = xread(fd, out, sizeof(out));
	check(r == 0, "basic: read on empty ring returns 0 (got %ld)", r);
	r = xwrite(fd, in, 0);
	check(r == 0, "basic: zero-length write returns 0 (got %ld)", r);

	reset(fd, 64);
	r = xwrite(fd, in, 100);
	check(r == 64, "basic: 100-byte write into 64-byte ring stores 64 (got %ld)", r);
	r = xwrite(fd, in, 1);
	check(r == -EAGAIN, "basic: write to full ring -> EAGAIN (got %ld)", r);
	s = stats(fd);
	check(s.capacity == 64 && s.used == 64, "basic: stats report capacity 64 used 64 (got %llu/%llu)",
	      (unsigned long long)s.capacity, (unsigned long long)s.used);
	xioctl(fd, RB_IOC_CLEAR, NULL);
	s = stats(fd);
	check(s.used == 0, "basic: CLEAR empties the ring");
}

static void t_wrap(int fd)
{
	uint8_t buf[64];
	uint64_t p = 0, c = 0;
	long bad = 0, iters = quick ? 4000 : 20000;

	reset(fd, 64);
	for (long i = 0; i < iters; i++) {
		size_t w = 1 + (i * 7) % 63, m = 1 + (i * 13) % 63;
		long r;

		for (size_t k = 0; k < w; k++)
			buf[k] = pat(p + k);
		r = xwrite(fd, buf, w);
		if (r > 0)
			p += r;
		else if (r != -EAGAIN)
			bad++;
		r = xread(fd, buf, m);
		if (r < 0) {
			bad++;
			continue;
		}
		for (long k = 0; k < r; k++)
			if (buf[k] != pat(c + k))
				bad++;
		c += r;
	}
	check(bad == 0 && stats(fd).used == p - c,
	      "wrap: %ld interleaved write/read cycles over the wrap point keep FIFO order (%ld errors)",
	      iters, bad);
}

static void t_peek(int fd)
{
	uint8_t buf[64];
	long bad = 0;

	/* Make the 60 stored bytes (pattern 40..99) wrap around the end. */
	reset(fd, 64);
	for (int k = 0; k < 64; k++)
		buf[k] = pat(k);
	xwrite(fd, buf, 50);
	xread(fd, buf, 40);
	for (int k = 0; k < 50; k++)
		buf[k] = pat(50 + k);
	xwrite(fd, buf, 50);

	for (uint64_t off = 0; off <= 60; off++) {
		uint64_t lens[] = { 0, 1, 7, 24, 60 - off };

		for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
			uint64_t len = lens[li];
			long r;

			if (off + len > 60)
				continue;
			memset(buf, 0xEE, sizeof(buf));
			r = peek(fd, off, len, buf);
			if (r != (long)len)
				bad++;
			for (uint64_t k = 0; k < len && r == (long)len; k++)
				if (buf[k] != pat(40 + off + k))
					bad++;
		}
	}
	check(bad == 0, "peek: every (offset, len) inside wrapped data returns the right bytes (%ld errors)", bad);
	check(stats(fd).used == 60, "peek: never consumes data");
}

static void t_hostile(int fd)
{
	static uint8_t big[4096];
	const struct { uint64_t off, len; } bad[] = {
		{ 11, 0 }, { 0, 11 }, { 10, 1 }, { 5, 6 }, { 64, 0 },
		{ UINT64_MAX, 1 }, { 1, UINT64_MAX }, { UINT64_MAX, UINT64_MAX },
		{ 1ULL << 63, 1ULL << 63 }, { UINT64_MAX - 4, 5 }, { 0, 4096 },
	};
	const uint64_t bad_caps[] = { 0, 1, 63, RB_MAX_CAPACITY + 1, 1ULL << 32, 1ULL << 60, UINT64_MAX };
	uint8_t ten[10];
	long r;

	reset(fd, 64);
	for (int k = 0; k < 10; k++)
		ten[k] = pat(k);
	xwrite(fd, ten, 10);

	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		r = peek(fd, bad[i].off, bad[i].len, big);
		check(r == -EINVAL, "hostile: PEEK offset=%#" PRIx64 " len=%#" PRIx64 " -> EINVAL (got %ld)",
		      bad[i].off, bad[i].len, r);
	}
	check(peek(fd, 10, 0, big) == 0, "hostile: PEEK at exact end with len 0 is allowed");
	check(peek(fd, 9, 1, big) == 1 && big[0] == pat(9), "hostile: PEEK of the last byte works");

	for (unsigned i = 0; i < sizeof(bad_caps) / sizeof(bad_caps[0]); i++) {
		r = resize(fd, bad_caps[i]);
		check(r == -EINVAL, "hostile: RESIZE %#" PRIx64 " -> EINVAL (got %ld)", bad_caps[i], r);
	}
	r = resize(fd, 128);
	check(r == -EBUSY, "hostile: RESIZE with data present -> EBUSY (got %ld)", r);
	check(stats(fd).used == 10 && xread(fd, ten, 10) == 10 && ten[3] == pat(3),
	      "hostile: data intact after all rejected calls");

	check(resize(fd, RB_MIN_CAPACITY) == 0, "hostile: RESIZE to the minimum (64) works");
	check(resize(fd, RB_MAX_CAPACITY) == 0, "hostile: RESIZE to the maximum (4 MiB) works");
	reset(fd, RB_DEFAULT_CAPACITY);
}

static void t_badptr(int fd)
{
	long pg = sysconf(_SC_PAGESIZE);
	uint8_t *ro, *none, *edge, buf[16];
	long r;

	ro = mmap(NULL, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	none = mmap(NULL, pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	edge = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mprotect(edge + pg, pg, PROT_NONE); /* first page usable, second faults */

	reset(fd, 4096);
	for (int k = 0; k < 10; k++)
		buf[k] = pat(k);
	xwrite(fd, buf, 10);

	check(xioctl(fd, RB_IOC_GET_STATS, null_ptr) == -EFAULT, "badptr: GET_STATS into NULL -> EFAULT");
	check(xioctl(fd, RB_IOC_GET_STATS, (void *)1) == -EFAULT, "badptr: GET_STATS into 0x1 -> EFAULT");
	check(xioctl(fd, RB_IOC_GET_STATS, KERNEL_ADDR) == -EFAULT, "badptr: GET_STATS into a kernel address -> EFAULT");
	check(xioctl(fd, RB_IOC_GET_STATS, ro) == -EFAULT, "badptr: GET_STATS into a read-only page -> EFAULT");
	check(xioctl(fd, RB_IOC_RESIZE, null_ptr) == -EFAULT, "badptr: RESIZE from NULL -> EFAULT");
	check(xioctl(fd, RB_IOC_PEEK, null_ptr) == -EFAULT, "badptr: PEEK request at NULL -> EFAULT");
	check(peek(fd, 0, 4, null_ptr) == -EFAULT, "badptr: PEEK into NULL -> EFAULT");
	check(peek(fd, 0, 4, KERNEL_ADDR) == -EFAULT, "badptr: PEEK into a kernel address -> EFAULT");
	check(peek(fd, 0, 4, ro) == -EFAULT, "badptr: PEEK into a read-only page -> EFAULT");

	check(xread(fd, null_ptr, 4) == -EFAULT, "badptr: read into NULL -> EFAULT");
	check(xread(fd, KERNEL_ADDR, 4) == -EFAULT, "badptr: read into a kernel address -> EFAULT");
	check(xread(fd, ro, 4) == -EFAULT, "badptr: read into a read-only page -> EFAULT");
	check(stats(fd).used == 10, "badptr: failed reads consumed nothing");

	/* A read whose buffer runs into an unmapped page: the copy may stop
	 * anywhere up to the 4 bytes that fit (copy_to_user may under-copy).
	 * Either a short count with exactly that much consumed, or EFAULT with
	 * nothing consumed, is correct. Losing or duplicating data is not. */
	r = xread(fd, edge + pg - 4, 10);
	{
		uint64_t used = stats(fd).used;
		bool okshort = r > 0 && r <= 4 && used == (uint64_t)(10 - r) &&
			       edge[pg - 4] == pat(0) && edge[pg - 4 + r - 1] == pat(r - 1);
		bool okfault = r == -EFAULT && used == 10;

		check(okshort || okfault,
		      "badptr: read straddling an unmapped page: short count or EFAULT, no data lost (got %ld, used %llu)",
		      r, (unsigned long long)used);
	}

	{
		uint64_t used = stats(fd).used;

		check(xwrite(fd, null_ptr, 4) == -EFAULT, "badptr: write from NULL -> EFAULT");
		check(xwrite(fd, none, 4) == -EFAULT, "badptr: write from a PROT_NONE page -> EFAULT");
		check(xwrite(fd, KERNEL_ADDR, 4) == -EFAULT, "badptr: write from a kernel address -> EFAULT");
		check(stats(fd).used == used, "badptr: failed writes stored nothing");
	}

	munmap(ro, pg);
	munmap(none, pg);
	munmap(edge, 2 * pg);
	reset(fd, RB_DEFAULT_CAPACITY);
}

static void t_ioctl(int fd)
{
	const unsigned long cmds[] = {
		_IO(RB_IOC_MAGIC, 0), _IO(RB_IOC_MAGIC, 5), _IO(RB_IOC_MAGIC, 0x7f),
		_IOR(RB_IOC_MAGIC, 1, uint32_t),	/* right number, wrong size */
		_IOW(RB_IOC_MAGIC, 3, uint32_t),
		_IO('T', 1), 0, 0xffffffffUL,
	};
	struct rb_stats junk;

	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		long r = xioctl(fd, cmds[i], &junk);

		check(r == -ENOTTY, "ioctl: unknown command %#lx -> ENOTTY (got %ld)", cmds[i], r);
	}
}

/* ---- concurrency -------------------------------------------------------- */

struct spsc {
	uint64_t total;
	_Atomic uint64_t produced, consumed;
	_Atomic long errors;
	_Atomic bool stop;
};

static void *spsc_producer(void *arg)
{
	struct spsc *t = arg;
	int fd = xopen();
	uint64_t s = 1, p = 0;
	uint8_t buf[4096];

	while (p < t->total && !t->stop) {
		size_t n = 1 + rnd(&s) % sizeof(buf);
		long r;

		if (n > t->total - p)
			n = t->total - p;
		for (size_t k = 0; k < n; k++)
			buf[k] = pat(p + k);
		r = xwrite(fd, buf, n);
		if (r > 0)
			p += r;
		else if (r == -EAGAIN)
			sched_yield();
		else
			t->errors++;
		t->produced = p;
	}
	close(fd);
	return NULL;
}

static void *spsc_consumer(void *arg)
{
	struct spsc *t = arg;
	int fd = xopen();
	uint64_t s = 2, c = 0, last = now_ns();
	uint8_t buf[4096];

	while (c < t->total) {
		long r = xread(fd, buf, 1 + rnd(&s) % sizeof(buf));

		if (r < 0) {
			t->errors++;
			break;
		}
		if (r == 0) {
			if (now_ns() - last > 20000000000ULL) { /* 20 s without progress */
				t->errors++;
				break;
			}
			sched_yield();
			continue;
		}
		for (long k = 0; k < r; k++)
			if (buf[k] != pat(c + k))
				t->errors++;
		c += r;
		last = now_ns();
		t->consumed = c;
	}
	t->stop = true;
	close(fd);
	return NULL;
}

static void t_spsc(int fd)
{
	struct spsc t = { .total = (quick ? 1 : 8) << 20 };
	pthread_t a, b;

	reset(fd, RB_DEFAULT_CAPACITY);
	pthread_create(&a, NULL, spsc_producer, &t);
	pthread_create(&b, NULL, spsc_consumer, &t);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	check(t.errors == 0 && t.consumed == t.total,
	      "spsc: producer and consumer threads stream %" PRIu64 " bytes, every byte in order (%ld errors)",
	      t.total, (long)t.errors);
}

struct mpmc {
	uint64_t per_writer;
	_Atomic uint64_t written, read;
	_Atomic int writers_left;
	_Atomic long errors, ioctl_calls;
};

static void *mpmc_writer(void *arg)
{
	struct mpmc *t = arg;
	int fd = xopen();
	uint64_t s = (uint64_t)pthread_self() | 1, done = 0;
	uint8_t buf[2048];

	memset(buf, 0x5A, sizeof(buf));
	while (done < t->per_writer) {
		size_t n = 1 + rnd(&s) % sizeof(buf);
		long r;

		if (n > t->per_writer - done)
			n = t->per_writer - done;
		r = xwrite(fd, buf, n);
		if (r > 0) {
			done += r;
			t->written += r;
		} else if (r == -EAGAIN) {
			sched_yield();
		} else {
			t->errors++;
		}
	}
	t->writers_left--;
	close(fd);
	return NULL;
}

static void *mpmc_reader(void *arg)
{
	struct mpmc *t = arg;
	int fd = xopen();
	uint64_t s = (uint64_t)pthread_self() | 3;
	uint8_t buf[2048];

	for (;;) {
		long r = xread(fd, buf, 1 + rnd(&s) % sizeof(buf));

		if (r < 0)
			t->errors++;
		else if (r > 0)
			t->read += r;
		else if (t->writers_left == 0 && stats(fd).used == 0)
			break;
		else
			sched_yield();
	}
	close(fd);
	return NULL;
}

static void *mpmc_ioctler(void *arg)
{
	struct mpmc *t = arg;
	int fd = xopen();
	uint8_t buf[64];

	while (t->writers_left > 0) {
		struct rb_stats s = stats(fd);
		uint64_t len = s.used < sizeof(buf) ? s.used : sizeof(buf);
		long r = peek(fd, 0, len, buf);

		/* Readers may drain the ring between GET_STATS and PEEK, so a
		 * clean EINVAL is fine; anything else is not. */
		if (r != (long)len && r != -EINVAL)
			t->errors++;
		t->ioctl_calls++;
	}
	close(fd);
	return NULL;
}

static void t_mpmc(int fd)
{
	enum { W = 4, R = 4 };
	struct mpmc t = { .per_writer = (quick ? 256 : 2048) << 10, .writers_left = W };
	struct rb_stats before, after;
	pthread_t th[W + R + 1];
	int n = 0;

	reset(fd, RB_DEFAULT_CAPACITY);
	before = stats(fd);
	for (int i = 0; i < W; i++)
		pthread_create(&th[n++], NULL, mpmc_writer, &t);
	for (int i = 0; i < R; i++)
		pthread_create(&th[n++], NULL, mpmc_reader, &t);
	pthread_create(&th[n++], NULL, mpmc_ioctler, &t);
	for (int i = 0; i < n; i++)
		pthread_join(th[i], NULL);
	after = stats(fd);

	check(t.errors == 0, "mpmc: %d writers, %d readers, 1 ioctl thread: no unexpected errors (%ld; %ld ioctls)",
	      W, R, (long)t.errors, (long)t.ioctl_calls);
	check(t.written == W * t.per_writer && t.read == t.written,
	      "mpmc: every byte written was read exactly once (%" PRIu64 " written, %" PRIu64 " read)",
	      (uint64_t)t.written, (uint64_t)t.read);
	check(after.bytes_written - before.bytes_written == t.written &&
	      after.bytes_read - before.bytes_read == t.read,
	      "mpmc: driver counters agree with what user space saw");
}

struct race {
	_Atomic bool stop;
	_Atomic long errors, resizes_ok, resizes_busy;
};

static void *race_io(void *arg)
{
	struct race *t = arg;
	int fd = xopen();
	uint8_t buf[512];

	memset(buf, 1, sizeof(buf));
	while (!t->stop) {
		long r = xwrite(fd, buf, sizeof(buf));

		if (r < 0 && r != -EAGAIN)
			t->errors++;
		r = xread(fd, buf, sizeof(buf));
		if (r < 0)
			t->errors++;
	}
	close(fd);
	return NULL;
}

static void t_race(int fd)
{
	struct race t = { 0 };
	const uint64_t caps[] = { 64, 4096, RB_DEFAULT_CAPACITY, RB_MAX_CAPACITY };
	pthread_t th[3];
	long iters = quick ? 300 : 2000;

	for (int i = 0; i < 3; i++)
		pthread_create(&th[i], NULL, race_io, &t);
	for (long i = 0; i < iters; i++) {
		long r;

		xioctl(fd, RB_IOC_CLEAR, NULL);
		r = resize(fd, caps[i % 4]);
		if (r == 0)
			t.resizes_ok++;
		else if (r == -EBUSY)
			t.resizes_busy++;
		else
			t.errors++;
	}
	t.stop = true;
	for (int i = 0; i < 3; i++)
		pthread_join(th[i], NULL);
	check(t.errors == 0, "race: CLEAR+RESIZE racing 3 I/O threads: only 0/EBUSY/EAGAIN seen (%ld ok, %ld busy, %ld bad)",
	      (long)t.resizes_ok, (long)t.resizes_busy, (long)t.errors);
	reset(fd, RB_DEFAULT_CAPACITY);
}

/* ---- main -------------------------------------------------------------- */

static const struct {
	const char *name;
	void (*fn)(int fd);
} groups[] = {
	{ "basic", t_basic }, { "wrap", t_wrap }, { "peek", t_peek },
	{ "hostile", t_hostile }, { "badptr", t_badptr }, { "ioctl", t_ioctl },
	{ "spsc", t_spsc }, { "mpmc", t_mpmc }, { "race", t_race },
};

int main(int argc, char **argv)
{
	const char *only = NULL;
	int fd;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--quick"))
			quick = true;
		else if (!strcmp(argv[i], "--only") && i + 1 < argc)
			only = argv[++i];
		else
			dev = argv[i];
	}
	if (!dev) {
		fprintf(stderr, "usage: ringtest <dev> [--quick] [--only group]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = xopen();
	for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		uint64_t t0;

		if (only && strcmp(only, groups[i].name))
			continue;
		t0 = now_ns();
		groups[i].fn(fd);
		printf("# group %s done in %.2f s\n", groups[i].name, (now_ns() - t0) / 1e9);
	}
	close(fd);
	printf("# ringtest %s: %s %d/%d\n", dev, nfail ? "FAIL" : "PASS",
	       nfail ? nfail : ntests, ntests);
	return nfail ? 1 : 0;
}
