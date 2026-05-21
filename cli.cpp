#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/limits.h>

#include "bunglefs.h"

class FdGuard {
public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() { if (fd_ >= 0) ::close(fd_); }
    int  get() const { return fd_; }
    bool ok()  const { return fd_ >= 0; }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
private:
    int fd_;
};

static int do_simple_ioctl(const char *path, unsigned long request)
{
    FdGuard fd(open(path, O_RDWR));
    if (!fd.ok()) { perror(path); return 1; }
    if (ioctl(fd.get(), request, 0) < 0) { perror("ioctl"); return 1; }
    return 0;
}

static int cmd_demo(const char *mountpoint)
{
    DIR *dir = opendir(mountpoint);
    if (!dir) { perror(mountpoint); return 1; }

    std::srand(static_cast<unsigned>(std::time(nullptr)) ^ static_cast<unsigned>(getpid()));

    int total = 0, ok = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", mountpoint, entry->d_name);
        total++;

        FdGuard fd(open(path, O_RDWR));
        if (!fd.ok()) { perror(path); continue; }

        unsigned int val_write = static_cast<unsigned int>(std::rand());
        unsigned int val_read  = 0;

        if (lseek(fd.get(), 0, SEEK_SET) < 0 ||
            write(fd.get(), &val_write, sizeof(val_write)) != (ssize_t)sizeof(val_write)) {
            fprintf(stderr, "%s: write: %s\n", path, strerror(errno));
            continue;
        }
        if (lseek(fd.get(), 0, SEEK_SET) < 0 ||
            read(fd.get(), &val_read, sizeof(val_read)) != (ssize_t)sizeof(val_read)) {
            fprintf(stderr, "%s: read: %s\n", path, strerror(errno));
            continue;
        }

        if (val_write == val_read) {
            printf("  %-20s  OK      (0x%08x)\n", entry->d_name, val_write);
            ok++;
        } else {
            printf("  %-20s  MISMATCH  wrote=0x%08x  read=0x%08x\n",
                   entry->d_name, val_write, val_read);
        }
    }

    closedir(dir);
    printf("\nИтог: %d / %d файлов прошли проверку\n", ok, total);
    return (ok == total && total > 0) ? 0 : 2;
}

static int cmd_list(const char *path)
{
    FdGuard fd(open(path, O_RDONLY));
    if (!fd.ok()) { perror(path); return 1; }

    bunglefs_meta_list lst{};
    if (ioctl(fd.get(), BUNGLEFS_IOC_LIST_META, &lst) < 0) {
        perror("ioctl LIST_META");
        return 1;
    }

    printf("Файлов: %u\n\n", lst.count);
    printf("  %-20s  %-12s  %-10s  %s\n", "Имя", "start_sector", "секторов", "hash");
    for (unsigned i = 0; i < lst.count; i++) {
        printf("  %-20s  %-12u  %-10u  0x%08x\n",
               lst.entries[i].name,
               lst.entries[i].start_sector,
               lst.entries[i].size_sectors,
               lst.entries[i].content_hash);
    }
    return 0;
}

static int cmd_map(const char *path, const char *name)
{
    FdGuard fd(open(path, O_RDONLY));
    if (!fd.ok()) { perror(path); return 1; }

    bunglefs_file_map fm{};
    strncpy(fm.name, name, BUNGLEFS_NAME_LEN - 1);

    if (ioctl(fd.get(), BUNGLEFS_IOC_FILE_MAP, &fm) < 0) {
        perror("ioctl FILE_MAP");
        return 1;
    }

    if (!fm.found) {
        fprintf(stderr, "Файл '%s' не найден\n", name);
        return 2;
    }

    printf("Файл: %s\n"
           "  start_sector : %u\n"
           "  end_sector   : %u\n"
           "  секторов     : %u\n"
           "  байт         : %u\n",
           name,
           fm.start_sector,
           fm.start_sector + fm.size_sectors - 1,
           fm.size_sectors,
           fm.size_sectors * BUNGLEFS_SECTOR_SIZE);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Использование:\n"
        "  %s demo  <точка_монтирования>\n"
        "  %s list  <файл_или_директория_в_ФС>\n"
        "  %s map   <файл_или_директория_в_ФС> <имя_файла>\n"
        "  %s zero  <файл_или_директория_в_ФС>\n"
        "  %s wipe  <файл_или_директория_в_ФС>\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *cmd  = argv[1];
    const char *path = argv[2];

    if (!std::strcmp(cmd, "demo")) return cmd_demo(path);
    if (!std::strcmp(cmd, "list")) return cmd_list(path);
    if (!std::strcmp(cmd, "zero")) return do_simple_ioctl(path, BUNGLEFS_IOC_ZERO_ALL);
    if (!std::strcmp(cmd, "wipe")) return do_simple_ioctl(path, BUNGLEFS_IOC_WIPE_FS);
    if (!std::strcmp(cmd, "map")) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_map(path, argv[3]);
    }

    fprintf(stderr, "Неизвестная команда: %s\n\n", cmd);
    usage(argv[0]);
    return 1;
}
