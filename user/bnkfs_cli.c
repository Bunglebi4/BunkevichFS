#include "../kernel/bnkfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static int grab(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		fd = open(path, O_RDONLY);
	if (fd < 0)
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
	return fd;
}

static int do_hashes(const char *mp)
{
	int fd = grab(mp);
	if (fd < 0)
		return 1;

	struct bfs_hashes req;
	memset(&req, 0, sizeof(req));

	if (ioctl(fd, BFS_IOC_HASHES, &req) < 0) {
		perror("ioctl GET_HASHES (probe)");
		close(fd);
		return 1;
	}
	uint32_t total = req.count;
	struct bfs_meta *rows = NULL;
	if (total) {
		rows = calloc(total, sizeof(*rows));
		if (!rows) { perror("calloc"); close(fd); return 1; }
	}
	req.max_count = total;
	req.entries   = (uint64_t)(uintptr_t)rows;
	if (ioctl(fd, BFS_IOC_HASHES, &req) < 0) {
		perror("ioctl GET_HASHES");
		free(rows);
		close(fd);
		return 1;
	}
	close(fd);

	printf("file_count=%u\n", req.count);
	for (uint32_t i = 0; i < req.count; i++)
		printf("%-20s start=%-10u count=%-6u crc32=0x%08x\n",
		       rows[i].name, rows[i].start_sector,
		       rows[i].sector_count, rows[i].hash);
	free(rows);
	return 0;
}

static int do_mapping(const char *mp, const char *name)
{
	int fd = grab(mp);
	if (fd < 0)
		return 1;

	struct bfs_map m;
	memset(&m, 0, sizeof(m));
	strncpy(m.name, name, sizeof(m.name) - 1);

	if (ioctl(fd, BFS_IOC_MAP, &m) < 0) {
		perror("ioctl GET_MAPPING");
		close(fd);
		return 1;
	}
	close(fd);
	printf("name=%s start_sector=%u nsectors=%u\n",
	       m.name, m.start_sector, m.sector_count);
	return 0;
}

static int do_zero(const char *mp)
{
	int fd = grab(mp);
	if (fd < 0)
		return 1;
	if (ioctl(fd, BFS_IOC_ZERO) < 0) {
		perror("ioctl ZERO_ALL");
		close(fd);
		return 1;
	}
	close(fd);
	printf("zero-all: OK\n");
	return 0;
}

static int do_erase(const char *mp)
{
	int fd = grab(mp);
	if (fd < 0)
		return 1;
	if (ioctl(fd, BFS_IOC_WIPE) < 0) {
		perror("ioctl ERASE_FS");
		close(fd);
		return 1;
	}
	close(fd);
	printf("erase: OK (обе копии SB обнулены)\n");
	return 0;
}

static void show_usage(const char *p)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <mountpoint> hashes\n"
		"  %s <mountpoint> mapping <filename>\n"
		"  %s <mountpoint> zero-all\n"
		"  %s <mountpoint> erase\n",
		p, p, p, p);
}

int main(int argc, char **argv)
{
	if (argc < 3) { show_usage(argv[0]); return 1; }
	const char *mp  = argv[1];
	const char *act = argv[2];

	if (!strcmp(act, "hashes"))   return do_hashes(mp);
	if (!strcmp(act, "zero-all")) return do_zero(mp);
	if (!strcmp(act, "erase"))    return do_erase(mp);
	if (!strcmp(act, "mapping")) {
		if (argc < 4) { show_usage(argv[0]); return 1; }
		return do_mapping(mp, argv[3]);
	}
	show_usage(argv[0]);
	return 1;
}
