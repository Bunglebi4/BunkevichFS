# BNKFS (учебная ФС для Linux 6.12.90)

Репозиторий содержит:
- модуль ядра `bnkfs.c` (регистрация ФС в VFS + работа поверх существующего блочного устройства);
- общий `ioctl` API: `bnkfs_ioctl.h`;
- userspace CLI-утилиту `bnkfs_tool` (`user_tool.cpp`) для проверки чтения/записи и вызова ioctl.

## Что реализовано

- 2 копии superblock:
  - основная по смещению `sb_primary_sector`;
  - резервная по смещению `sb_backup_sector`.
- Контроль целостности superblock через `crc32`.
- При первой инициализации:
  - вычисляется доступное пространство после `max(primary, backup)`;
  - создаются все файлы сразу;
  - каждый файл имеет одинаковый размер: `max_file_sectors` секторов.
- Файлы доступны в корне монтирования (`/mnt`), операции:
  - чтение;
  - запись.
- Остальные операции (create/rename/unlink/mkdir и т.д.) не реализованы.
- Управление через `/dev/bnkfs_ctl`:
  - `BNKFS_IOC_ZERO_ALL`;
  - `BNKFS_IOC_ERASE_FS`;
  - `BNKFS_IOC_GET_HASHES`;
  - `BNKFS_IOC_GET_MAP`.

## Сборка

```bash
make
```

Будет собрано:
- модуль `bnkfs.ko`;
- userspace утилита `./bnkfs_tool`.

## Пример запуска

Ниже пример с loop-устройством.

```bash
# 1) создать образ
dd if=/dev/zero of=/tmp/bnkfs.img bs=1M count=32

# 2) подключить loop device
sudo losetup -fP /tmp/bnkfs.img
losetup -a | rg bnkfs.img
# допустим, получили /dev/loop0

# 3) загрузить модуль
sudo insmod bnkfs.ko \
  disk_name=/dev/loop0 \
  sb_primary_sector=0 \
  sb_backup_sector=16 \
  max_filename_len=32 \
  max_file_sectors=2

# 4) смонтировать
sudo mkdir -p /mnt
sudo mount -t bnkfs /dev/loop0 /mnt

# 5) посмотреть файлы
ls /mnt
```

## Проверка userspace утилитой

```bash
# запись случайного числа в каждый файл + чтение и проверка
sudo ./bnkfs_tool --demo --mount /mnt

# ioctl: список метаинформации (хэши)
sudo ./bnkfs_tool --hashes

# ioctl: получить маппинг секторов файла
sudo ./bnkfs_tool --map f000000000

# ioctl: обнулить все файлы
sudo ./bnkfs_tool --zero

# ioctl: стереть всю ФС на устройстве
sudo ./bnkfs_tool --erase
```

## Размонтирование и выгрузка

```bash
sudo umount /mnt
sudo rmmod bnkfs
sudo losetup -d /dev/loop0
```

