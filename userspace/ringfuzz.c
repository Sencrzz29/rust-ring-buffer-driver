// SPDX-License-Identifier: GPL-2.0
/*
 * ringfuzz - random-operation fuzzer for a ring-buffer device.
 *
 *   ringfuzz <dev> <iterations> [seed]
 *
 * Each iteration picks a random operation -- read, write, PEEK, RESIZE,
 * CLEAR, GET_STATS, an unknown ioctl, or close+reopen -- with arguments
 * biased towards the values that break drivers: 0, 1, powers of two and
 * their neighbours, 2^31, 2^32, 2^63, 2^64-1, and bad pointers (NULL, 0x1,
 * kernel addresses, unmapped, read-only, PROT_NONE, page-straddling).
 *
 * After every operation it checks:
 *   - the return value is a success or an errno the ABI allows for that op;
 *   - GET_STATS still works and capacity is within [64 B, 4 MiB],
 *     used <= capacity;
 *   - a successful PEEK returned exactly `len` bytes and len <= used.
 *
 * A correct driver survives with 0 violations. A naive one usually takes
 * the kernel down long before the end -- which is the point.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <uapi/ringbuf.h>

enum { OP_WRITE, OP_READ, OP_PEEK, OP_RESIZE, OP_CLEAR, OP_STATS, OP_BOGUS, OP_REOPEN, NOPS };
static const char *opname[NOPS] = { "write", "read", "peek", "resize", "clear", "stats", "bogus", "reopen" };
/* relative frequency of each op */
static const int weight[NOPS] = { 20, 20, 25, 8, 4, 8, 8, 2 };

static uint64_t seed_state;
static uint64_t rnd(void)
{
	uint64_t x = seed_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	seed_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t interesting(void)
{
	static const uint64_t v[] = {
		0, 1, 2, 3, 7, 8, 15, 16, 63, 64, 65, 127, 128, 255, 256, 4095, 4096, 4097,
		65535, 65536, 65537, RB_MAX_CAPACITY - 1, RB_MAX_CAPACITY, RB_MAX_CAPACITY + 1,
		0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, 0x100000000ULL,
		1ULL << 60, 1ULL << 62, (1ULL << 63) - 1, 1ULL << 63, UINT64_MAX - 1, UINT64_MAX,
	};

	switch (rnd() % 4) {
	case 0:
		return rnd();
	case 1:
		return rnd() % 8192;
	default:
		return v[rnd() % (sizeof(v) / sizeof(v[0]))];
	}
}

static uint8_t *heap, *ro, *none, *edge, *gone;
static long pg;

/* Returns a pointer of a random kind; *valid_len is how many bytes are
 * actually writable there (0 for bad pointers). */
static void *pick_ptr(size_t *valid_len, bool want_writable)
{
	switch (rnd() % 10) {
	case 0: *valid_len = 0; return NULL;
	case 1: *valid_len = 0; return (void *)1;
	case 2: *valid_len = 0; return (void *)0xffff000000000000ULL;
	case 3: *valid_len = 0; return gone;			/* unmapped */
	case 4: *valid_len = 0; return none;			/* PROT_NONE */
	case 5: *valid_len = want_writable ? 0 : (size_t)pg;	/* read-only */
		return ro;
	case 6: *valid_len = 16; return edge + pg - 16;		/* straddles PROT_NONE */
	default: *valid_len = 8192; return heap;
	}
}

static bool allowed(int op, long r)
{
	if (r >= 0)
		return true;
	switch (op) {
	case OP_WRITE:	return r == -EAGAIN || r == -EFAULT;
	case OP_READ:	return r == -EFAULT;
	case OP_PEEK:	return r == -EINVAL || r == -EFAULT;
	case OP_RESIZE:	return r == -EINVAL || r == -EBUSY || r == -ENOMEM || r == -EFAULT;
	case OP_STATS:	return r == -EFAULT;
	case OP_BOGUS:	return r == -ENOTTY;
	default:	return false;
	}
}

int main(int argc, char **argv)
{
	long counts[NOPS] = { 0 }, ok[NOPS] = { 0 }, errs[NOPS][140] = { { 0 } };
	long iters, violations = 0, wsum = 0;
	const char *dev;
	int fd;

	if (argc < 3) {
		fprintf(stderr, "usage: ringfuzz <dev> <iterations> [seed]\n");
		return 2;
	}
	dev = argv[1];
	iters = atol(argv[2]);
	seed_state = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x1234567;
	if (!seed_state)
		seed_state = 1;
	for (int i = 0; i < NOPS; i++)
		wsum += weight[i];

	pg = sysconf(_SC_PAGESIZE);
	heap = malloc(8192);
	memset(heap, 0xA5, 8192);
	ro = mmap(NULL, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	none = mmap(NULL, pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	edge = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mprotect(edge + pg, pg, PROT_NONE);
	gone = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munmap(gone, pg);

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("# ringfuzz %s iterations=%ld seed=%#" PRIx64 "\n", dev, iters, seed_state);

	for (long it = 0; it < iters; it++) {
		long pick = (long)(rnd() % (uint64_t)wsum), r = 0;
		int op = 0;
		size_t vlen;
		void *p;

		while (pick >= weight[op])
			pick -= weight[op++];
		counts[op]++;

		switch (op) {
		case OP_WRITE: {
			size_t n = rnd() % 2 ? rnd() % 8192 : interesting() % (1 << 20);

			p = pick_ptr(&vlen, false);
			if (p == heap && n > vlen)
				n = vlen;
			r = write(fd, p, n);
			break;
		}
		case OP_READ: {
			size_t n = rnd() % 2 ? rnd() % 8192 : interesting() % (1 << 20);

			p = pick_ptr(&vlen, true);
			if (p == heap && n > vlen)
				n = vlen;
			r = read(fd, p, n);
			break;
		}
		case OP_PEEK: {
			struct rb_peek q = { .offset = interesting(), .len = interesting() };
			struct rb_stats s;

			p = pick_ptr(&vlen, true);
			/* keep the "valid buffer" case actually valid for small lens */
			if (p == heap && q.len > vlen && rnd() % 2)
				q.len %= vlen + 1;
			q.buf = (uintptr_t)p;
			r = ioctl(fd, RB_IOC_PEEK, rnd() % 50 ? (void *)&q : NULL);
			if (r >= 0 && (ioctl(fd, RB_IOC_GET_STATS, &s) || (uint64_t)r != q.len ||
				       q.offset + q.len > s.used || q.offset + q.len < q.offset)) {
				printf("VIOLATION it=%ld: PEEK off=%#llx len=%#llx returned %ld\n", it,
				       (unsigned long long)q.offset, (unsigned long long)q.len, r);
				violations++;
			}
			break;
		}
		case OP_RESIZE: {
			uint64_t cap = interesting();

			r = ioctl(fd, RB_IOC_RESIZE, rnd() % 50 ? (void *)&cap : NULL);
			break;
		}
		case OP_CLEAR:
			r = ioctl(fd, RB_IOC_CLEAR);
			break;
		case OP_STATS: {
			struct rb_stats s;

			p = rnd() % 3 ? (void *)&s : pick_ptr(&vlen, true);
			r = ioctl(fd, RB_IOC_GET_STATS, p);
			break;
		}
		case OP_BOGUS: {
			unsigned long cmd;

			switch (rnd() % 3) {
			case 0: cmd = _IOC(rnd() % 4, RB_IOC_MAGIC, 5 + rnd() % 250, rnd() % 256); break;
			case 1: cmd = _IOC(rnd() % 4, RB_IOC_MAGIC, 1 + rnd() % 4, 1 + rnd() % 64); break;
			default: cmd = 0x802c542a; break; /* TCGETS2: what isatty() sends */
			}
			/* a known command number with the right size is not bogus */
			if (cmd == RB_IOC_GET_STATS || cmd == RB_IOC_CLEAR ||
			    cmd == RB_IOC_RESIZE || cmd == RB_IOC_PEEK)
				cmd = _IO(RB_IOC_MAGIC, 0x7f);
			r = ioctl(fd, cmd, heap);
			break;
		}
		case OP_REOPEN:
			close(fd);
			fd = open(dev, O_RDWR);
			if (fd < 0) {
				perror("reopen");
				return 2;
			}
			r = 0;
			break;
		}

		if (r < 0)
			r = -errno;
		if (r >= 0)
			ok[op]++;
		else if (-r < 140)
			errs[op][-r]++;
		if (!allowed(op, r)) {
			printf("VIOLATION it=%ld: %s returned %ld (%s)\n", it, opname[op], r, strerror((int)-r));
			violations++;
		}

		/* invariants, every iteration */
		{
			struct rb_stats s;

			if (ioctl(fd, RB_IOC_GET_STATS, &s)) {
				printf("VIOLATION it=%ld: GET_STATS failed: %s\n", it, strerror(errno));
				violations++;
			} else if (s.capacity < RB_MIN_CAPACITY || s.capacity > RB_MAX_CAPACITY ||
				   s.used > s.capacity) {
				printf("VIOLATION it=%ld: capacity %llu used %llu\n", it,
				       (unsigned long long)s.capacity, (unsigned long long)s.used);
				violations++;
			}
		}
		if (violations > 20) {
			printf("# too many violations, stopping\n");
			break;
		}
		if (it && it % 20000 == 0)
			printf("# progress %ld/%ld, violations %ld\n", it, iters, violations);
	}

	for (int op = 0; op < NOPS; op++) {
		printf("# %-7s calls=%-6ld ok=%-6ld", opname[op], counts[op], ok[op]);
		for (int e = 1; e < 140; e++)
			if (errs[op][e])
				printf(" %s=%ld", strerrorname_np(e) ? strerrorname_np(e) : "?", errs[op][e]);
		putchar('\n');
	}
	printf("# ringfuzz %s: %s iterations=%ld violations=%ld\n", dev,
	       violations ? "FAIL" : "PASS", iters, violations);
	close(fd);
	return violations ? 1 : 0;
}
