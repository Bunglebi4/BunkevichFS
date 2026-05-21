/*
 * bnkfs_test.c — демонстрационная программа из задания.
 *
 * Обходит все файлы внутри переданного mountpoint, в каждый пишет
 * 64-битное случайное число и тут же читает его обратно. В конце —
 * сводка "N passed, M failed".
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

/* Собираем список имён файлов вида file_* в каталоге. */
static char **collect_files(const char *dir, size_t *out_n)
{
	DIR *d = opendir(dir);
	if (!d) { fprintf(stderr, "opendir(%s): %s\n", dir, strerror(errno)); return NULL; }

	size_t cap = 64, n = 0;
	char **arr = malloc(cap * sizeof(*arr));
	if (!arr) { closedir(d); return NULL; }

	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "file_", 5) != 0)
			continue;
		if (n == cap) {
			cap *= 2;
			char **tmp = realloc(arr, cap * sizeof(*arr));
			if (!tmp) { closedir(d); free(arr); return NULL; }
			arr = tmp;
		}
		arr[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(arr, n, sizeof(*arr), cmp_str);
	*out_n = n;
	return arr;
}

/* Простой 64-битный ГПСЧ на базе xorshift, чтобы не таскать libstdc++. */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 0xdeadbeefULL; }
static uint64_t rng_next(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <mountpoint>\n", argv[0]);
		return 1;
	}
	const char *mp = argv[1];

	size_t n;
	char **names = collect_files(mp, &n);
	if (!names || n == 0) {
		fprintf(stderr, "no files found in %s\n", mp);
		return 1;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	rng_seed((uint64_t)ts.tv_sec ^ (uint64_t)ts.tv_nsec ^ (uint64_t)getpid());

	size_t passed = 0, failed = 0;
	char path[4096];

	for (size_t i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", mp, names[i]);
		int fd = open(path, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
			failed++;
			continue;
		}
		uint64_t value = rng_next();
		if (pwrite(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
			fprintf(stderr, "%s: pwrite: %s\n", path, strerror(errno));
			close(fd); failed++; continue;
		}
		fsync(fd);

		uint64_t back = 0;
		if (pread(fd, &back, sizeof(back), 0) != (ssize_t)sizeof(back)) {
			fprintf(stderr, "%s: pread: %s\n", path, strerror(errno));
			close(fd); failed++; continue;
		}
		close(fd);

		if (back == value) {
			printf("%s: OK  (0x%016llx)\n",
			       names[i], (unsigned long long)value);
			passed++;
		} else {
			printf("%s: FAIL  wrote=0x%llx read=0x%llx\n",
			       names[i],
			       (unsigned long long)value,
			       (unsigned long long)back);
			failed++;
		}
	}

	for (size_t i = 0; i < n; i++) free(names[i]);
	free(names);

	printf("%zu passed, %zu failed\n", passed, failed);
	return failed ? 2 : 0;
}
