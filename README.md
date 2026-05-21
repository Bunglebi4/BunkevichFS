# BunkevichFS

Учебная файловая система для ядра Linux **6.12.x**. Размещается поверх блочного
устройства, хранит две CRC32-защищённые копии суперблока, при инициализации
создаёт детерминированный набор файлов одинакового размера. Из VFS поддержаны
только `read`/`write`. Кроме того модуль предоставляет четыре собственных IOCTL.

## Состав репозитория

| Путь | Что внутри |
|------|------------|
| [`kernel/`](kernel/) | Исходники ядерного модуля и его `Makefile` |
| [`kernel/bnkfs.h`](kernel/bnkfs.h) | Общий заголовок (IOCTL, структуры) для ядра и userspace |
| [`kernel/bnkfs_internal.h`](kernel/bnkfs_internal.h) | Внутренний заголовок ядра |
| [`kernel/super.c`](kernel/super.c) | Init модуля, `fill_super`, CRC, форматирование |
| [`kernel/inode.c`](kernel/inode.c) | Корневой каталог и файловые inode, `iterate_shared`/`lookup` |
| [`kernel/file.c`](kernel/file.c) | `file_operations`, диспетчер IOCTL и его обработчики |
| [`user/`](user/) | Userspace-утилиты и их `Makefile` |
| [`user/bnkfs_cli.c`](user/bnkfs_cli.c) | CLI к четырём IOCTL |
| [`user/bnkfs_test.c`](user/bnkfs_test.c) | Round-trip: случайная запись/чтение по всем файлам |
| [`scripts/verify.sh`](scripts/verify.sh) | End-to-end проверка (15 шагов) |
| [`Makefile`](Makefile) | Верхнеуровневая сборка |

## Раскладка на диске

```
sector sb1_offset         основной суперблок          (512 байт, CRC32)
sector sb2_offset         резервная копия суперблока  (байт-в-байт)
...   file_0000  ...      M секторов
...   file_0001  ...      M секторов
...                       ...
```

`sb1_offset` и `sb2_offset` задаются параметрами модуля. Данные располагаются
сразу после самой дальней копии SB. Имена файлов формируются автоматически:
`file_NNNN`. Никакой отдельной таблицы inode на диске нет — всё восстанавливается
по содержимому суперблока.

## Параметры модуля

| Параметр            | Тип    | Default | Назначение                                        |
|---------------------|--------|---------|---------------------------------------------------|
| `disk_name`         | charp  | `""`    | Ожидаемое короткое имя блочного устройства        |
| `sb1_offset`        | uint   | `0`     | Сектор основной копии суперблока                  |
| `sb2_offset`        | uint   | `16`    | Сектор резервной копии (должен ≠ `sb1_offset`)    |
| `max_name_len`      | uint   | `32`    | Максимальная длина имени файла                    |
| `max_file_sectors`  | uint   | `4`     | Размер каждого файла в 512-байтных секторах (M)   |

## Сборка

```bash
make                       # kernel + user
make -C kernel KDIR=/path/to/linux-6.12.90    # против конкретного дерева ядра
make clean
```

Получаем `kernel/bnkfs.ko`, `user/bnkfs_cli`, `user/bnkfs_test`.

## Быстрый старт

```bash
# 1. Создаём loop-устройство.
dd if=/dev/zero of=/tmp/bnkfs.img bs=1M count=4
sudo losetup -fP /tmp/bnkfs.img    # появится /dev/loopN

# 2. Грузим модуль.
sudo insmod kernel/bnkfs.ko \
    disk_name=loopN sb1_offset=0 sb2_offset=32 \
    max_name_len=32 max_file_sectors=8

# 3. Монтируем.
sudo mkdir -p /mnt/bnkfs
sudo mount -t bnkfs /dev/loopN /mnt/bnkfs
ls /mnt/bnkfs

# 4. Round-trip тест.
sudo ./user/bnkfs_test /mnt/bnkfs

# 5. IOCTL.
sudo ./user/bnkfs_cli /mnt/bnkfs hashes
sudo ./user/bnkfs_cli /mnt/bnkfs mapping file_0003
sudo ./user/bnkfs_cli /mnt/bnkfs zero-all
sudo ./user/bnkfs_cli /mnt/bnkfs erase
```

## Полная верификация

```bash
sudo bash scripts/verify.sh
```

Проверяет 15 шагов: сборку, параметры модуля, mount + автоформат, размеры файлов,
round-trip, все четыре IOCTL, восстановление основного SB из копии, восстановление
после порчи копии, ERASE, валидацию параметров.
