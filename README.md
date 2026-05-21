# BunkevichFS (SimpleFS)

Учебный модуль ядра Linux: простая ФС поверх блочного устройства с
двумя копиями superblock-а, целостность которых защищена CRC32.

Цель — домашнее задание курса. Версия ядра: **6.12.x**.

## Состав

| Файл           | Назначение                                    |
|----------------|-----------------------------------------------|
| `bunglefs.h`   | общие определения (ioctl, layout)             |
| `bunglefs.c`   | модуль ядра                                   |
| `cli.c`        | userspace CLI / демо-программа                |
| `Makefile`     | сборка модуля и CLI                           |

## Сборка

```
make            # собирает bunglefs.ko и bunglefs_cli
```

Заголовки ядра должны лежать в `/lib/modules/$(uname -r)/build`.

## Подготовка тестового диска

Для разработки удобно использовать loopback-устройство:

```
dd if=/dev/zero of=disk.img bs=1M count=16
sudo losetup /dev/loop0 disk.img
```

## Параметры модуля

| Параметр            | По умолчанию  | Назначение                              |
|---------------------|---------------|------------------------------------------|
| `disk_name`         | `/dev/loop0`  | путь к блочному устройству              |
| `sb1_offset`        | `0`           | сектор первой копии superblock-а        |
| `sb2_offset`        | `1024`        | сектор второй копии superblock-а        |
| `max_name_len`      | `32`          | максимальная длина имени файла          |
| `max_file_sectors`  | `4`           | M — максимальный размер файла в секторах |

## Загрузка и монтирование

```
sudo insmod ./bunglefs.ko \
    disk_name=/dev/loop0 sb1_offset=0 sb2_offset=1024 \
    max_name_len=16 max_file_sectors=4

sudo mkdir -p /mnt/bunglefs
sudo mount -t bunglefs none /mnt/bunglefs

ls /mnt/bunglefs                 # увидите file0, file1, ...
```

При первом монтировании ФС форматируется автоматически (записывает обе
копии SB, размечает файлы). При повторном монтировании читается primary
SB, проверяется его CRC32; если она не сходится — восстановление из
backup-а; если оба плохи — повторное форматирование.

## Демо-программа

```
sudo ./bunglefs_cli demo /mnt/bunglefs
```

Программа обходит все файлы, пишет в каждый случайное число и читает
его обратно, выводит сравнение.

## IOCTL

```
sudo ./bunglefs_cli zero /mnt/bunglefs/file0     # обнулить все файлы
sudo ./bunglefs_cli list /mnt/bunglefs/file0     # вывести метаинформацию
sudo ./bunglefs_cli map  /mnt/bunglefs/file0 file3   # маппинг секторов
sudo ./bunglefs_cli wipe /mnt/bunglefs/file0     # стереть FS
```

После `wipe` нужно размонтировать и заново загрузить модуль /
смонтировать ФС, чтобы переформатировать диск.

## Размонтирование и выгрузка

```
sudo umount /mnt/bunglefs
sudo rmmod bunglefs
sudo losetup -d /dev/loop0
```

## Layout на диске

```
sector 0                : SuperBlock (копия 1)            ─┐
sector sb1+span..       : файлы file0, file1, ...          │
        ...                                                ├── размечается
sector sb2_offset       : SuperBlock (копия 2)             │   при mount-е
sector sb2+span..       : продолжение файлов до конца диска ┘
```

SB занимает несколько секторов (по `sizeof(struct bunglefs_dsb)`), при
расстановке файлов область второй копии SB пропускается.
