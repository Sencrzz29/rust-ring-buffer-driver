// SPDX-License-Identifier: GPL-2.0
/*
 * ringbench - throughput and latency of one or more ring-buffer devices.
 *
 *   ringbench [-n pairs] [-r runs] [-b spsc_MiB] [-L lat_samples] dev...
 *
 * Measurements (all against a freshly cleared 64 KiB ring):
 *   rw    single thread: `pairs` x (write chunk; read chunk), for chunk in
 *         16, 64, 256, 1024, 4096 bytes. Reports total ns -> MB/s, ns/op.
 *   spsc  producer + consumer thread stream `spsc_MiB` MiB through the
 *         device in 512 B and 4096 B chunks (EAGAIN -> sched_yield).
 *   lat   per-operation latency of one 64-byte write+read pair:
 *         median and 99th percentile over `lat_samples` samples.
 *
 * Devices are measured in alternating order every run (A B, then B A, ...)
 * so slow drift of the emulator affects both equally.
 *
 * Output: CSV lines prefixed "CSV," for the host-side parser:
 *   CSV,kind,dev,chunk,run,ops,bytes,ns
 * (for lat: ops = samples, bytes = p50 ns, ns = p99 ns)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <uapi/ringbuf.h>

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int open_fresh(const char *dev)
{
	uint64_t cap = RB_DEFAULT_CAPACITY;
	int fd = open(dev, O_RDWR);

	if (fd < 0) {
		perror(dev);
		exit(2);
	}
	ioctl(fd, RB_IOC_CLEAR);
	if (ioctl(fd, RB_IOC_RESIZE, &cap)) {
		perror("resize");
		exit(2);
	}
	return fd;
}

static void die_short(const char *what, ssize_t r, size_t want)
{
	fprintf(stderr, "%s: got %zd, wanted %zu (%s)\n", what, r, want, strerror(errno));
	exit(3);
}

static void bench_rw(const char *dev, size_t chunk, long pairs, int run)
{
	static uint8_t buf[4096];
	int fd = open_fresh(dev);
	uint64_t t0, t1;

	for (long i = 0; i < 200; i++) { /* warm-up */
		if (write(fd, buf, chunk) != (ssize_t)chunk || read(fd, buf, chunk) != (ssize_t)chunk)
			die_short("warm-up", -1, chunk);
	}
	t0 = now_ns();
	for (long i = 0; i < pairs; i++) {
		ssize_t r = write(fd, buf, chunk);

		if (r != (ssize_t)chunk)
			die_short("write", r, chunk);
		r = read(fd, buf, chunk);
		if (r != (ssize_t)chunk)
			die_short("read", r, chunk);
	}
	t1 = now_ns();
	printf("CSV,rw,%s,%zu,%d,%ld,%llu,%llu\n", dev, chunk, run, pairs,
	       (unsigned long long)(2ULL * pairs * chunk), (unsigned long long)(t1 - t0));
	close(fd);
}

struct spsc { const char *dev; size_t chunk; uint64_t total; };

static void *producer(void *arg)
{
	struct spsc *t = arg;
	static uint8_t buf[4096];
	int fd = open(t->dev, O_RDWR);
	uint64_t done = 0;

	while (done < t->total) {
		ssize_t r = write(fd, buf, t->chunk);

		if (r > 0)
			done += r;
		else if (errno == EAGAIN)
			sched_yield();
		else
			die_short("spsc write", r, t->chunk);
	}
	close(fd);
	return NULL;
}

static void bench_spsc(const char *dev, size_t chunk, uint64_t total, int run)
{
	static uint8_t buf[4096];
	struct spsc t = { dev, chunk, total };
	int fd = open_fresh(dev);
	uint64_t got = 0, t0, t1;
	pthread_t th;

	t0 = now_ns();
	pthread_create(&th, NULL, producer, &t);
	while (got < total) {
		ssize_t r = read(fd, buf, chunk);

		if (r > 0)
			got += r;
		else if (r == 0)
			sched_yield();
		else
			die_short("spsc read", r, chunk);
	}
	pthread_join(th, NULL);
	t1 = now_ns();
	printf("CSV,spsc,%s,%zu,%d,%llu,%llu,%llu\n", dev, chunk, run,
	       (unsigned long long)(total / chunk), (unsigned long long)total,
	       (unsigned long long)(t1 - t0));
	close(fd);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void bench_lat(const char *dev, long samples, int run)
{
	uint64_t *ns = calloc(samples, sizeof(*ns));
	uint8_t buf[64];
	int fd = open_fresh(dev);

	for (long i = 0; i < samples; i++) {
		uint64_t t0 = now_ns();

		if (write(fd, buf, 64) != 64 || read(fd, buf, 64) != 64)
			die_short("lat", -1, 64);
		ns[i] = now_ns() - t0;
	}
	qsort(ns, samples, sizeof(*ns), cmp_u64);
	printf("CSV,lat,%s,64,%d,%ld,%llu,%llu\n", dev, run, samples,
	       (unsigned long long)ns[samples / 2], (unsigned long long)ns[samples * 99 / 100]);
	free(ns);
	close(fd);
}

int main(int argc, char **argv)
{
	const size_t chunks[] = { 16, 64, 256, 1024, 4096 };
	const size_t spsc_chunks[] = { 512, 4096 };
	long pairs = 20000, lat_samples = 20000;
	int runs = 5, opt, ndev;
	uint64_t spsc_mib = 16;
	char **devs;

	while ((opt = getopt(argc, argv, "n:r:b:L:")) != -1) {
		switch (opt) {
		case 'n': pairs = atol(optarg); break;
		case 'r': runs = atoi(optarg); break;
		case 'b': spsc_mib = strtoull(optarg, NULL, 0); break;
		case 'L': lat_samples = atol(optarg); break;
		default:
			fprintf(stderr, "usage: ringbench [-n pairs] [-r runs] [-b spsc_MiB] [-L samples] dev...\n");
			return 2;
		}
	}
	devs = argv + optind;
	ndev = argc - optind;
	if (ndev < 1) {
		fprintf(stderr, "no devices\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("# ringbench pairs=%ld runs=%d spsc=%" PRIu64 "MiB lat_samples=%ld\n",
	       pairs, runs, spsc_mib, lat_samples);

	for (int run = 1; run <= runs; run++) {
		for (int k = 0; k < ndev; k++) {
			/* alternate the device order every run */
			const char *dev = devs[run % 2 ? k : ndev - 1 - k];

			for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++)
				bench_rw(dev, chunks[c], pairs, run);
			for (unsigned c = 0; c < sizeof(spsc_chunks) / sizeof(spsc_chunks[0]); c++)
				bench_spsc(dev, spsc_chunks[c], spsc_mib << 20, run);
			bench_lat(dev, lat_samples, run);
		}
		printf("# run %d/%d done\n", run, runs);
	}
	return 0;
}
