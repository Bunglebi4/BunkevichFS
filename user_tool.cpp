// user_tool.cpp — демонстрационная утилита для BunkevichFS.
//
// Поддерживает:
//   user_tool roundtrip <mountpoint>   — обходит все файлы, в каждый пишет
//                                        случайное число и читает его обратно.
//   user_tool list     <mountpoint>    — показывает имена/хэши всех файлов.
//   user_tool mapping  <file>          — выводит маппинг секторов для файла.
//   user_tool zero     <mountpoint>    — IOCTL "обнулить все файлы".
//   user_tool erase    <mountpoint>    — IOCTL "стереть ФС".
//
// <mountpoint> здесь — это любой существующий файл/директория внутри ФС
// (например, /mnt или /mnt/file_0000). IOCTL диспетчер общий для всех инодов.

#include "bnkfs_ioctl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <random>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

namespace {

int open_for_ioctl(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        fd = ::open(path.c_str(), O_RDONLY); // если это файл
    if (fd < 0) {
        std::perror(("open " + path).c_str());
    }
    return fd;
}

// Перечисляем все файлы вида file_XXXX в директории mount-поинта.
std::vector<std::string> list_files(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) { std::perror(("opendir " + dir).c_str()); return out; }
    while (struct dirent* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n.rfind("file_", 0) == 0) out.push_back(n);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Кладёт 8-байтное случайное число в начало файла и читает его обратно.
int cmd_roundtrip(const std::string& mp) {
    auto names = list_files(mp);
    if (names.empty()) {
        std::cerr << "В " << mp << " не найдено ни одного файла file_*\n";
        return 1;
    }
    std::mt19937_64 rng(std::random_device{}());
    size_t ok = 0, bad = 0;
    for (const auto& n : names) {
        std::string path = mp + "/" + n;
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) { std::perror(("open " + path).c_str()); ++bad; continue; }

        uint64_t value = rng();
        if (::pwrite(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
            std::perror("pwrite"); ::close(fd); ++bad; continue;
        }
        // fsync чтобы убедиться, что данные ушли через VFS на устройство.
        ::fsync(fd);

        uint64_t back = 0;
        if (::pread(fd, &back, sizeof(back), 0) != (ssize_t)sizeof(back)) {
            std::perror("pread"); ::close(fd); ++bad; continue;
        }
        ::close(fd);

        if (back == value) {
            std::printf("%s: OK  (0x%016llx)\n", n.c_str(),
                        (unsigned long long)value);
            ++ok;
        } else {
            std::printf("%s: FAIL  wrote=0x%llx read=0x%llx\n",
                        n.c_str(),
                        (unsigned long long)value,
                        (unsigned long long)back);
            ++bad;
        }
    }
    std::printf("Итого: %zu OK, %zu FAIL\n", ok, bad);
    return bad == 0 ? 0 : 2;
}

int cmd_list(const std::string& mp) {
    int fd = open_for_ioctl(mp);
    if (fd < 0) return 1;

    // Сначала узнаём, сколько всего файлов.
    struct bnkfs_hashes_req req{};
    req.max_count = 0;
    req.entries   = 0;
    if (::ioctl(fd, BNKFS_IOC_GET_HASHES, &req) < 0) {
        std::perror("ioctl GET_HASHES (probe)"); ::close(fd); return 1;
    }
    uint32_t total = req.count;
    std::vector<struct bnkfs_file_meta> arr(total);
    req.max_count = total;
    req.entries   = (uint64_t)(uintptr_t)arr.data();
    if (::ioctl(fd, BNKFS_IOC_GET_HASHES, &req) < 0) {
        std::perror("ioctl GET_HASHES"); ::close(fd); return 1;
    }
    ::close(fd);

    std::printf("Найдено файлов: %u\n", req.count);
    std::printf("%-20s %-12s %-10s %-10s\n", "name", "hash", "start", "count");
    for (uint32_t i = 0; i < req.count && i < arr.size(); i++) {
        std::printf("%-20s 0x%08x  %-10u %-10u\n",
                    arr[i].name, arr[i].hash,
                    arr[i].start_sector, arr[i].sector_count);
    }
    return 0;
}

int cmd_mapping(const std::string& filepath) {
    int fd = open_for_ioctl(filepath);
    if (fd < 0) return 1;

    // Берём только базовое имя файла (последняя компонента пути).
    std::string base = filepath;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);

    struct bnkfs_mapping m{};
    std::strncpy(m.name, base.c_str(), sizeof(m.name) - 1);
    if (::ioctl(fd, BNKFS_IOC_GET_MAPPING, &m) < 0) {
        std::perror("ioctl GET_MAPPING"); ::close(fd); return 1;
    }
    ::close(fd);
    std::printf("Файл %s: секторы [%u .. %u] (всего %u)\n",
                m.name, m.start_sector,
                m.start_sector + m.sector_count - 1,
                m.sector_count);
    return 0;
}

int cmd_zero(const std::string& mp) {
    int fd = open_for_ioctl(mp);
    if (fd < 0) return 1;
    if (::ioctl(fd, BNKFS_IOC_ZERO_ALL) < 0) {
        std::perror("ioctl ZERO_ALL"); ::close(fd); return 1;
    }
    ::close(fd);
    std::printf("Все файлы обнулены.\n");
    return 0;
}

int cmd_erase(const std::string& mp) {
    int fd = open_for_ioctl(mp);
    if (fd < 0) return 1;
    if (::ioctl(fd, BNKFS_IOC_ERASE_FS) < 0) {
        std::perror("ioctl ERASE_FS"); ::close(fd); return 1;
    }
    ::close(fd);
    std::printf("ФС стёрта (суперблоки обнулены). Размонтируйте и убедитесь, "
                "что повторный mount пересоздаст её.\n");
    return 0;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "Использование:\n"
        "  %s roundtrip <mountpoint>\n"
        "  %s list      <mountpoint>\n"
        "  %s mapping   <path/to/file_XXXX>\n"
        "  %s zero      <mountpoint>\n"
        "  %s erase     <mountpoint>\n",
        argv0, argv0, argv0, argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 1; }
    std::string cmd = argv[1];
    std::string arg = argv[2];

    if (cmd == "roundtrip") return cmd_roundtrip(arg);
    if (cmd == "list")      return cmd_list(arg);
    if (cmd == "mapping")   return cmd_mapping(arg);
    if (cmd == "zero")      return cmd_zero(arg);
    if (cmd == "erase")     return cmd_erase(arg);

    usage(argv[0]);
    return 1;
}
