/*
 * bnkfs_cli.c — командная утилита для IOCTL BunkevichFS.
 *
 * Использование:
 *   bnkfs_cli <mountpoint> hashes
 *   bnkfs_cli <mountpoint> mapping <filename>
 *   bnkfs_cli <mountpoint> zero-all
 *   bnkfs_cli <mountpoint> erase
 *
 * mountpoint — любой существующий объект внутри ФС (директория или файл),
 * IOCTL общий для всех инодов.
 */

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

/* Открываем директорию или файл — IOCTL работает на любом иноде ФС. */
static int open_target(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		fd = open(path, O_RDONLY);
	if (fd < 0)
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
	return fd;
}

static int cmd_hashes(const char *mp)
{
	int fd = open_target(mp);
	if (fd < 0)
		return 1;

	struct bnkfs_hashes_req req;
	memset(&req, 0, sizeof(req));

	/* Первый вызов — узнать file_count, не пересылая буфер. */
	if (ioctl(fd, BNKFS_IOC_GET_HASHES, &req) < 0) {
		perror("ioctl GET_HASHES (probe)");
		close(fd);
		return 1;
	}
	uint32_t total = req.count;
	struct bnkfs_file_meta *arr = NULL;
	if (total) {
		arr = calloc(total, sizeof(*arr));
		if (!arr) { perror("calloc"); close(fd); return 1; }
	}
	req.max_count = total;
	req.entries   = (uint64_t)(uintptr_t)arr;
	if (ioctl(fd, BNKFS_IOC_GET_HASHES, &req) < 0) {
		perror("ioctl GET_HASHES");
		free(arr);
		close(fd);
		return 1;
	}
	close(fd);

	printf("file_count=%u\n", req.count);
	for (uint32_t i = 0; i < req.count; i++)
		printf("%-20s start=%-10u count=%-6u crc32=0x%08x\n",
		       arr[i].name, arr[i].start_sector,
		       arr[i].sector_count, arr[i].hash);
	free(arr);
	return 0;
}

static int cmd_mapping(const char *mp, const char *name)
{
	int fd = open_target(mp);
	if (fd < 0)
		return 1;

	struct bnkfs_mapping m;
	memset(&m, 0, sizeof(m));
	strncpy(m.name, name, sizeof(m.name) - 1);

	if (ioctl(fd, BNKFS_IOC_GET_MAPPING, &m) < 0) {
		perror("ioctl GET_MAPPING");
		close(fd);
		return 1;
	}
	close(fd);
	printf("name=%s start_sector=%u nsectors=%u\n",
	       m.name, m.start_sector, m.sector_count);
	return 0;
}

static int cmd_zero_all(const char *mp)
{
	int fd = open_target(mp);
	if (fd < 0)
		return 1;
	if (ioctl(fd, BNKFS_IOC_ZERO_ALL) < 0) {
		perror("ioctl ZERO_ALL");
		close(fd);
		return 1;
	}
	close(fd);
	printf("zero-all: OK\n");
	return 0;
}

static int cmd_erase(const char *mp)
{
	int fd = open_target(mp);
	if (fd < 0)
		return 1;
	if (ioctl(fd, BNKFS_IOC_ERASE_FS) < 0) {
		perror("ioctl ERASE_FS");
		close(fd);
		return 1;
	}
	close(fd);
	printf("erase: OK (обе копии SB обнулены)\n");
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <mountpoint> hashes\n"
		"  %s <mountpoint> mapping <filename>\n"
		"  %s <mountpoint> zero-all\n"
		"  %s <mountpoint> erase\n",
		argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	if (argc < 3) { usage(argv[0]); return 1; }
	const char *mp  = argv[1];
	const char *cmd = argv[2];

	if (!strcmp(cmd, "hashes"))   return cmd_hashes(mp);
	if (!strcmp(cmd, "zero-all")) return cmd_zero_all(mp);
	if (!strcmp(cmd, "erase"))    return cmd_erase(mp);
	if (!strcmp(cmd, "mapping")) {
		if (argc < 4) { usage(argv[0]); return 1; }
		return cmd_mapping(mp, argv[3]);
	}
	usage(argv[0]);
	return 1;
}
