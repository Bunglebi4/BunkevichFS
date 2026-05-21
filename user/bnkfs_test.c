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

static int by_str(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

static char **scan_dir(const char *dir, size_t *out_n)
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
	qsort(arr, n, sizeof(*arr), by_str);
	*out_n = n;
	return arr;
}

static uint64_t g_state;
static void rng_init(uint64_t s) { g_state = s ? s : 0xdeadbeefULL; }
static uint64_t rng_pull(void)
{
	uint64_t x = g_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	g_state = x;
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
	char **list = scan_dir(mp, &n);
	if (!list || n == 0) {
		fprintf(stderr, "no files found in %s\n", mp);
		return 1;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	rng_init((uint64_t)ts.tv_sec ^ (uint64_t)ts.tv_nsec ^ (uint64_t)getpid());

	size_t good = 0, bad = 0;
	char path[4096];

	for (size_t i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", mp, list[i]);
		int fd = open(path, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
			bad++;
			continue;
		}
		uint64_t value = rng_pull();
		if (pwrite(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
			fprintf(stderr, "%s: pwrite: %s\n", path, strerror(errno));
			close(fd); bad++; continue;
		}
		fsync(fd);

		uint64_t echo = 0;
		if (pread(fd, &echo, sizeof(echo), 0) != (ssize_t)sizeof(echo)) {
			fprintf(stderr, "%s: pread: %s\n", path, strerror(errno));
			close(fd); bad++; continue;
		}
		close(fd);

		if (echo == value) {
			printf("%s: OK  (0x%016llx)\n",
			       list[i], (unsigned long long)value);
			good++;
		} else {
			printf("%s: FAIL  wrote=0x%llx read=0x%llx\n",
			       list[i],
			       (unsigned long long)value,
			       (unsigned long long)echo);
			bad++;
		}
	}

	for (size_t i = 0; i < n; i++) free(list[i]);
	free(list);

	printf("%zu passed, %zu failed\n", good, bad);
	return bad ? 2 : 0;
}
