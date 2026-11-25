// SPDX-License-Identifier: GPL-2.0
/*
 * ringctl - poke a ring-buffer device's ioctls from the shell.
 *
 *   ringctl <dev> stats                  print struct rb_stats
 *   ringctl <dev> clear                  RB_IOC_CLEAR
 *   ringctl <dev> resize <bytes>         RB_IOC_RESIZE
 *   ringctl <dev> peek <offset> <len>    RB_IOC_PEEK, raw bytes to stdout
 *   ringctl <dev> stats-badptr           GET_STATS into an invalid pointer
 *   ringctl <dev> bogus                  an ioctl number the driver doesn't know
 *
 * Numbers accept decimal or 0x hex and may be up to 2^64-1, so hostile
 * values can be passed straight through to the driver.
 *
 * Exit status: 0 on success, otherwise the errno the driver returned
 * (e.g. 22 = EINVAL, 14 = EFAULT, 16 = EBUSY, 25 = ENOTTY), which lets
 * shell tests assert on exact error codes.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/ringbuf.h>

static int fail(const char *what)
{
	int e = errno;

	fprintf(stderr, "ringctl: %s: %s (errno %d)\n", what, strerror(e), e);
	return e ? e : 1;
}

static uint64_t num(const char *s)
{
	char *end;
	uint64_t v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || *end) {
		fprintf(stderr, "ringctl: bad number '%s'\n", s);
		exit(2);
	}
	return v;
}

int main(int argc, char **argv)
{
	const char *cmd;
	int fd;

	if (argc < 3) {
		fprintf(stderr, "usage: ringctl <dev> stats|clear|resize N|peek OFF LEN|stats-badptr|bogus\n");
		return 2;
	}
	cmd = argv[2];

	fd = open(argv[1], O_RDWR);
	if (fd < 0)
		return fail("open");

	if (!strcmp(cmd, "stats")) {
		struct rb_stats s;

		if (ioctl(fd, RB_IOC_GET_STATS, &s))
			return fail("GET_STATS");
		printf("capacity %" PRIu64 " used %" PRIu64 " written %" PRIu64
		       " read %" PRIu64 " rejected %" PRIu64 "\n",
		       (uint64_t)s.capacity, (uint64_t)s.used, (uint64_t)s.bytes_written,
		       (uint64_t)s.bytes_read, (uint64_t)s.rejected);
	} else if (!strcmp(cmd, "clear")) {
		if (ioctl(fd, RB_IOC_CLEAR))
			return fail("CLEAR");
	} else if (!strcmp(cmd, "resize") && argc == 4) {
		__u64 cap = num(argv[3]);

		if (ioctl(fd, RB_IOC_RESIZE, &cap))
			return fail("RESIZE");
	} else if (!strcmp(cmd, "peek") && argc == 5) {
		struct rb_peek p = { .offset = num(argv[3]), .len = num(argv[4]) };
		/* Only allocate what could legitimately be returned; for a hostile
		 * length the driver must reject the request before touching buf. */
		size_t alloc = p.len <= RB_MAX_CAPACITY ? (size_t)p.len : 16;
		char *buf = malloc(alloc ? alloc : 1);
		long n;

		if (!buf)
			return fail("malloc");
		p.buf = (uintptr_t)buf;
		n = ioctl(fd, RB_IOC_PEEK, &p);
		if (n < 0)
			return fail("PEEK");
		fwrite(buf, 1, (size_t)n, stdout);
		free(buf);
	} else if (!strcmp(cmd, "stats-badptr")) {
		if (ioctl(fd, RB_IOC_GET_STATS, (void *)1))
			return fail("GET_STATS(bad pointer)");
	} else if (!strcmp(cmd, "bogus")) {
		if (ioctl(fd, _IO(RB_IOC_MAGIC, 0x7f)))
			return fail("bogus ioctl");
	} else {
		fprintf(stderr, "ringctl: unknown command '%s'\n", cmd);
		return 2;
	}
	close(fd);
	return 0;
}
