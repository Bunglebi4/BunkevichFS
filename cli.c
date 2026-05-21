/*
 * BunkevichFS userspace CLI / demo
 *
 *   bunglefs_cli demo <mount-point>
 *       walk all files, write random number then read it back, verify
 *
 *   bunglefs_cli zero    <any-file-in-fs>
 *   bunglefs_cli wipe    <any-file-in-fs>
 *   bunglefs_cli list    <any-file-in-fs>
 *   bunglefs_cli map     <any-file-in-fs> <filename>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "bunglefs.h"

static int do_ioctl_simple(const char *path, unsigned long req)
{
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, req, 0) < 0) { perror("ioctl"); close(fd); return 1; }
	close(fd);
	return 0;
}

static int cmd_list(const char *path)
{
	struct bunglefs_meta_list list;
	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	memset(&list, 0, sizeof(list));
	if (ioctl(fd, BUNGLEFS_IOC_LIST_META, &list) < 0) {
		perror("ioctl LIST_META"); close(fd); return 1;
	}
	close(fd);
	printf("Files: %u\n", list.count);
	for (unsigned i = 0; i < list.count; i++) {
		printf("  %-16s start=%-8u size=%-4u hash=0x%08x\n",
			list.entries[i].name,
			list.entries[i].start_sector,
			list.entries[i].size_sectors,
			list.entries[i].content_hash);
	}
	return 0;
}

static int cmd_map(const char *path, const char *name)
{
	struct bunglefs_file_map fm;
	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	memset(&fm, 0, sizeof(fm));
	strncpy(fm.name, name, BUNGLEFS_NAME_LEN - 1);
	if (ioctl(fd, BUNGLEFS_IOC_FILE_MAP, &fm) < 0) {
		perror("ioctl FILE_MAP"); close(fd); return 1;
	}
	close(fd);
	if (!fm.found) { printf("not found: %s\n", name); return 2; }
	printf("%s: sectors [%u .. %u]  (%u sectors, %u bytes)\n",
		name, fm.start_sector,
		fm.start_sector + fm.size_sectors - 1,
		fm.size_sectors,
		fm.size_sectors * BUNGLEFS_SECTOR_SIZE);
	return 0;
}

static int cmd_demo(const char *mountpoint)
{
	DIR *d = opendir(mountpoint);
	struct dirent *e;
	int total = 0, ok = 0;

	if (!d) { perror("opendir"); return 1; }
	srand(time(NULL) ^ getpid());

	while ((e = readdir(d))) {
		char fp[PATH_MAX];
		int fd;
		unsigned int wrote, readv = 0;

		if (e->d_name[0] == '.') continue;
		snprintf(fp, sizeof(fp), "%s/%s", mountpoint, e->d_name);

		total++;
		fd = open(fp, O_RDWR);
		if (fd < 0) { perror(fp); continue; }

		wrote = (unsigned)rand();
		if (lseek(fd, 0, SEEK_SET) < 0 ||
		    write(fd, &wrote, sizeof(wrote)) != sizeof(wrote)) {
			fprintf(stderr, "%s: write failed: %s\n", fp, strerror(errno));
			close(fd); continue;
		}
		if (lseek(fd, 0, SEEK_SET) < 0 ||
		    read(fd, &readv, sizeof(readv)) != sizeof(readv)) {
			fprintf(stderr, "%s: read failed: %s\n", fp, strerror(errno));
			close(fd); continue;
		}
		close(fd);

		if (wrote == readv) {
			printf("  %-16s OK  (0x%08x)\n", e->d_name, wrote);
			ok++;
		} else {
			printf("  %-16s MISMATCH  wrote=0x%08x read=0x%08x\n",
				e->d_name, wrote, readv);
		}
	}
	closedir(d);
	printf("Done: %d/%d OK\n", ok, total);
	return (ok == total && total > 0) ? 0 : 2;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s demo <mount-point>\n"
		"  %s zero <file-in-fs>\n"
		"  %s wipe <file-in-fs>\n"
		"  %s list <file-in-fs>\n"
		"  %s map  <file-in-fs> <name>\n",
		prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	if (argc < 3) { usage(argv[0]); return 1; }

	if (!strcmp(argv[1], "demo"))  return cmd_demo(argv[2]);
	if (!strcmp(argv[1], "zero"))  return do_ioctl_simple(argv[2], BUNGLEFS_IOC_ZERO_ALL);
	if (!strcmp(argv[1], "wipe"))  return do_ioctl_simple(argv[2], BUNGLEFS_IOC_WIPE_FS);
	if (!strcmp(argv[1], "list"))  return cmd_list(argv[2]);
	if (!strcmp(argv[1], "map")) {
		if (argc < 4) { usage(argv[0]); return 1; }
		return cmd_map(argv[2], argv[3]);
	}
	usage(argv[0]);
	return 1;
}
