#!/bin/bash
set -e

IMG_FILE=${IMG_FILE:-/tmp/bunglefs.img}
MNT=${MNT:-/mnt}
SB1=${SB1:-0}
SB2=${SB2:-1024}
M=${M:-4}

echo "[1/6] Создаём образ диска $IMG_FILE (8M)..."
dd if=/dev/zero of="$IMG_FILE" bs=1M count=8 status=none

echo "[2/6] Подключаем loop-устройство..."
LOOP=$(losetup --find --show "$IMG_FILE")
echo "      $LOOP"

echo "[3/6] Загружаем модуль bunglefs..."
insmod bunglefs.ko disk_name="$LOOP" sb1_offset="$SB1" sb2_offset="$SB2" max_file_sectors="$M"

echo "[4/6] Монтируем ФС в $MNT..."
mount -t bunglefs none "$MNT"
ls -la "$MNT"

echo ""
echo "[5/6] Тест запись/чтение..."
./bunglefs_cli demo "$MNT"
echo ""

echo "[6/6] Метаинформация файлов:"
./bunglefs_cli list "$MNT"

echo ""
echo "Нажмите Enter для размонтирования..."
read -r

umount "$MNT"
rmmod bunglefs
losetup -d "$LOOP"
echo "Готово."
