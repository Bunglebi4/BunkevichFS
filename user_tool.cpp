#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "bnkfs_ioctl.h"

namespace fs = std::filesystem;

struct Options {
	std::string mount_dir = "/mnt";
	std::string ctl_dev = "/dev/bnkfs_ctl";
	bool demo = false;
	bool zero = false;
	bool erase = false;
	bool hashes = false;
	bool map = false;
	std::string map_name;
};

static void print_usage(const char *prog)
{
	std::cerr
		<< "Usage:\n"
		<< "  " << prog << " --demo [--mount /mnt] [--ctl /dev/bnkfs_ctl]\n"
		<< "  " << prog << " --zero [--ctl /dev/bnkfs_ctl]\n"
		<< "  " << prog << " --erase [--ctl /dev/bnkfs_ctl]\n"
		<< "  " << prog << " --hashes [--ctl /dev/bnkfs_ctl]\n"
		<< "  " << prog << " --map <filename> [--ctl /dev/bnkfs_ctl]\n";
}

static bool parse_args(int argc, char **argv, Options &opt)
{
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--mount" && i + 1 < argc) {
			opt.mount_dir = argv[++i];
		} else if (a == "--ctl" && i + 1 < argc) {
			opt.ctl_dev = argv[++i];
		} else if (a == "--demo") {
			opt.demo = true;
		} else if (a == "--zero") {
			opt.zero = true;
		} else if (a == "--erase") {
			opt.erase = true;
		} else if (a == "--hashes") {
			opt.hashes = true;
		} else if (a == "--map" && i + 1 < argc) {
			opt.map = true;
			opt.map_name = argv[++i];
		} else {
			return false;
		}
	}
	return true;
}

static int open_ctl(const std::string &path)
{
	int fd = open(path.c_str(), O_RDWR);
	if (fd < 0)
		std::cerr << "open(" << path << ") failed: " << std::strerror(errno) << "\n";
	return fd;
}

static int run_demo(const std::string &mount_dir)
{
	std::vector<fs::directory_entry> entries;
	std::mt19937_64 rng(std::random_device{}());

	for (const auto &entry : fs::directory_iterator(mount_dir)) {
		if (entry.is_regular_file())
			entries.push_back(entry);
	}
	std::sort(entries.begin(), entries.end(),
		  [](const auto &a, const auto &b) { return a.path().filename() < b.path().filename(); });

	if (entries.empty()) {
		std::cerr << "no files in " << mount_dir << "\n";
		return 1;
	}

	for (const auto &entry : entries) {
		uint64_t value = rng();
		uint64_t read_back = 0;
		auto p = entry.path();

		{
			std::ofstream out(p, std::ios::binary | std::ios::in | std::ios::out);
			if (!out) {
				std::cerr << "write open failed for " << p << "\n";
				return 1;
			}
			out.write(reinterpret_cast<const char *>(&value), sizeof(value));
			if (!out) {
				std::cerr << "write failed for " << p << "\n";
				return 1;
			}
		}

		{
			std::ifstream in(p, std::ios::binary);
			if (!in) {
				std::cerr << "read open failed for " << p << "\n";
				return 1;
			}
			in.read(reinterpret_cast<char *>(&read_back), sizeof(read_back));
			if (!in) {
				std::cerr << "read failed for " << p << "\n";
				return 1;
			}
		}

		if (read_back != value) {
			std::cerr << "mismatch in " << p.filename() << ": wrote " << value
				  << ", read " << read_back << "\n";
			return 1;
		}
	}

	std::cout << "demo check passed, files tested: " << entries.size() << "\n";
	return 0;
}

static int do_zero(int fd)
{
	if (ioctl(fd, BNKFS_IOC_ZERO_ALL) < 0) {
		std::cerr << "ioctl ZERO_ALL failed: " << std::strerror(errno) << "\n";
		return 1;
	}
	std::cout << "all files were zeroed\n";
	return 0;
}

static int do_erase(int fd)
{
	if (ioctl(fd, BNKFS_IOC_ERASE_FS) < 0) {
		std::cerr << "ioctl ERASE_FS failed: " << std::strerror(errno) << "\n";
		return 1;
	}
	std::cout << "filesystem erased on device\n";
	return 0;
}

static int do_hashes(int fd)
{
	bnkfs_ioctl_hash_list list_req {};
	std::vector<bnkfs_ioctl_hash_entry> entries(256);

	list_req.entries_ptr = reinterpret_cast<uint64_t>(entries.data());
	list_req.capacity = static_cast<uint32_t>(entries.size());

	if (ioctl(fd, BNKFS_IOC_GET_HASHES, &list_req) < 0) {
		std::cerr << "ioctl GET_HASHES failed: " << std::strerror(errno) << "\n";
		return 1;
	}

	if (list_req.total_count > entries.size()) {
		entries.resize(list_req.total_count);
		list_req.entries_ptr = reinterpret_cast<uint64_t>(entries.data());
		list_req.capacity = static_cast<uint32_t>(entries.size());
		if (ioctl(fd, BNKFS_IOC_GET_HASHES, &list_req) < 0) {
			std::cerr << "ioctl GET_HASHES(2) failed: " << std::strerror(errno) << "\n";
			return 1;
		}
	}

	for (uint32_t i = 0; i < list_req.count; i++) {
		std::cout << entries[i].name << " start=" << entries[i].start_sector
			  << " sectors=" << entries[i].sectors
			  << " hash=0x" << std::hex << entries[i].hash << std::dec << "\n";
	}
	std::cout << "total files: " << list_req.total_count << "\n";
	return 0;
}

static int do_map(int fd, const std::string &name)
{
	bnkfs_ioctl_sector_map_req req {};

	std::strncpy(req.name, name.c_str(), BNKFS_IOCTL_NAME_MAX - 1);
	if (ioctl(fd, BNKFS_IOC_GET_MAP, &req) < 0) {
		std::cerr << "ioctl GET_MAP failed: " << std::strerror(errno) << "\n";
		return 1;
	}
	std::cout << req.name << " -> start_sector=" << req.start_sector
		  << ", sectors=" << req.sectors << "\n";
	return 0;
}

int main(int argc, char **argv)
{
	Options opt {};
	int ret = 0;

	if (!parse_args(argc, argv, opt)) {
		print_usage(argv[0]);
		return 1;
	}

	if (opt.demo)
		ret = run_demo(opt.mount_dir);
	if (ret)
		return ret;

	if (opt.zero || opt.erase || opt.hashes || opt.map) {
		int fd = open_ctl(opt.ctl_dev);
		if (fd < 0)
			return 1;

		if (opt.zero)
			ret = do_zero(fd);
		if (!ret && opt.erase)
			ret = do_erase(fd);
		if (!ret && opt.hashes)
			ret = do_hashes(fd);
		if (!ret && opt.map)
			ret = do_map(fd, opt.map_name);

		close(fd);
	}

	if (!opt.demo && !opt.zero && !opt.erase && !opt.hashes && !opt.map) {
		print_usage(argv[0]);
		return 1;
	}

	return ret;
}
