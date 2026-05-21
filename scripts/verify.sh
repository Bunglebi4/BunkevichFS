#!/usr/bin/env bash
# Полная локальная проверка BunkevichFS. Запускать от root:
#   sudo bash scripts/verify.sh
#
# Скрипт не падает на первой ошибке — он считает PASS/FAIL по каждому шагу
# и выводит сводку в конце. Покрывает все юзкейсы задания.

set -u
cd "$(dirname "$0")/.." || exit 1

IMG=/tmp/bnkfs_verify.img
MNT=/mnt/bnkfs_verify
LOOP=""
PASS=0
FAIL=0
FAILED_STEPS=()

SB1=0
SB2=32
NAME_LEN=32
M_SECTORS=8

step() { echo; echo "=== STEP $1: $2"; }
ok()   { echo "STATUS: OK   ($1)"; PASS=$((PASS+1)); }
fail() { echo "STATUS: FAIL ($1)"; FAIL=$((FAIL+1)); FAILED_STEPS+=("$1"); }

cleanup() {
    echo; echo "=== CLEANUP"
    mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
    lsmod | grep -q '^bnkfs ' && rmmod bnkfs 2>/dev/null
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null
    rm -f "$IMG"
}
trap cleanup EXIT

if [[ $(id -u) -ne 0 ]]; then
    echo "должен запускаться от root (sudo bash scripts/verify.sh)"
    exit 1
fi

# ------------------------------------------------------------------- #
step 0 "сборка модуля и userspace"
make clean >/dev/null 2>&1
if make >/tmp/bnkfs_build.log 2>&1; then
    ls kernel/bnkfs.ko user/bnkfs_cli user/bnkfs_test
    ok "build"
else
    tail -30 /tmp/bnkfs_build.log
    fail "build"; exit 1
fi

# ------------------------------------------------------------------- #
step 1 "modinfo показывает все 5 параметров модуля"
parms=$(modinfo kernel/bnkfs.ko | grep -c '^parm:')
echo "parm lines: $parms"
[[ "$parms" -eq 5 ]] && ok "modinfo" || fail "modinfo"

# ------------------------------------------------------------------- #
step 2 "создаём 4 МиБ образ и привязываем к свободному loop"
truncate -s 4M "$IMG"
LOOP=$(losetup -f --show "$IMG")
echo "LOOP=$LOOP"
size=$(blockdev --getsz "$LOOP")
echo "size in sectors: $size"
[[ "$size" -eq 8192 ]] && ok "loopdev" || fail "loopdev"

# ------------------------------------------------------------------- #
step 3 "загружаем модуль"
LOOPNAME=$(basename "$LOOP")
if insmod kernel/bnkfs.ko disk_name="$LOOPNAME" \
       sb1_offset=$SB1 sb2_offset=$SB2 \
       max_name_len=$NAME_LEN max_file_sectors=$M_SECTORS 2>&1; then
    lsmod | grep '^bnkfs '
    ok "insmod"
else
    fail "insmod"; exit 1
fi

# ------------------------------------------------------------------- #
step 4 "mount: ожидаем авто-форматирование"
mkdir -p "$MNT"
dmesg -c >/dev/null 2>&1 || true
if mount -t bnkfs "$LOOP" "$MNT" 2>&1; then
    dmesg | tail -3
    count=$(ls "$MNT" | wc -l)
    echo "file count: $count"
    [[ "$count" -gt 0 ]] && ok "mount" || fail "mount"
else
    fail "mount"; exit 1
fi
dmesg | tail -5 | grep -q "форматирую" && ok "format_log" || fail "format_log"

# ------------------------------------------------------------------- #
step 5 "stat первый/последний файл — должны быть M*512 байт"
first=$(ls "$MNT" | sort | head -1)
last=$(ls "$MNT"  | sort | tail -1)
fsz=$(stat -c '%s' "$MNT/$first")
lsz=$(stat -c '%s' "$MNT/$last")
expected=$((M_SECTORS * 512))
echo "$first: $fsz; $last: $lsz; expected=$expected"
[[ "$fsz" -eq "$expected" && "$lsz" -eq "$expected" ]] \
    && ok "stat" || fail "stat"

# ------------------------------------------------------------------- #
step 6 "bnkfs_test: случайная запись/чтение каждого файла"
out=$(./user/bnkfs_test "$MNT" | tail -1)
echo "$out"
echo "$out" | grep -q "0 failed" && ok "test" || fail "test"

# ------------------------------------------------------------------- #
step 7 "ioctl mapping: file_0000 начинается с data_start"
m0=$(./user/bnkfs_cli "$MNT" mapping file_0000)
m1=$(./user/bnkfs_cli "$MNT" mapping file_0001)
echo "$m0"
echo "$m1"
data_start=$((SB2 + 1))
expected0="start_sector=$data_start nsectors=$M_SECTORS"
expected1="start_sector=$((data_start + M_SECTORS)) nsectors=$M_SECTORS"
echo "$m0" | grep -q "$expected0" && ok "mapping_f0" || fail "mapping_f0"
echo "$m1" | grep -q "$expected1" && ok "mapping_f1" || fail "mapping_f1"

# ------------------------------------------------------------------- #
step 8 "ioctl mapping для несуществующего файла -> ENOENT"
err=$(./user/bnkfs_cli "$MNT" mapping nope 2>&1 || true)
echo "$err"
echo "$err" | grep -qi "No such file" && ok "mapping_enoent" || fail "mapping_enoent"

# ------------------------------------------------------------------- #
step 9 "ioctl hashes: первая строка file_count, остальные — записи"
hdr=$(./user/bnkfs_cli "$MNT" hashes | head -1)
lines=$(./user/bnkfs_cli "$MNT" hashes | wc -l)
echo "header: $hdr"
echo "lines : $lines"
count=$(ls "$MNT" | wc -l)
[[ "$hdr" == "file_count=$count" && "$lines" -eq $((count + 1)) ]] \
    && ok "hashes_count" || fail "hashes_count"

# ------------------------------------------------------------------- #
step 10 "ioctl zero-all: первый файл должен стать нулевым"
echo "non-zero" > "$MNT/$first"; sync
./user/bnkfs_cli "$MNT" zero-all
sync
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
hex=$(head -c 16 "$MNT/$first" | xxd -p)
echo "first 16 bytes: $hex"
[[ "$hex" == "00000000000000000000000000000000" ]] \
    && ok "zero_all" || fail "zero_all"

# ------------------------------------------------------------------- #
step 11 "после zero-all у всех файлов один и тот же crc32"
uniq_crcs=$(./user/bnkfs_cli "$MNT" hashes | tail -n +2 \
            | awk '{print $NF}' | sort -u | wc -l)
echo "unique crc32 values: $uniq_crcs"
[[ "$uniq_crcs" -eq 1 ]] && ok "hashes_after_zero" || fail "hashes_after_zero"

# ------------------------------------------------------------------- #
step 12 "ломаем основной SB, перемонтируем, ожидаем восстановление из копии"
echo "persistent-payload" > "$MNT/$first"; sync
umount "$MNT"
dd if=/dev/zero of="$LOOP" bs=512 count=1 seek=$SB1 conv=notrunc status=none
mag1_before=$(xxd -l 4 -s $((SB1 * 512)) "$LOOP" | awk '{print $2$3}')
mag2_before=$(xxd -l 4 -s $((SB2 * 512)) "$LOOP" | awk '{print $2$3}')
echo "magic primary before: $mag1_before"
echo "magic backup before : $mag2_before"
dmesg -c >/dev/null 2>&1 || true
mount -t bnkfs "$LOOP" "$MNT" 2>&1
dmesg | tail -3
mag1_after=$(xxd -l 4 -s $((SB1 * 512)) "$LOOP" | awk '{print $2$3}')
echo "magic primary after : $mag1_after"
[[ "$mag1_after" == "464b4e42" ]] && ok "repair_primary" || fail "repair_primary"
content=$(head -c 18 "$MNT/$first")
[[ "$content" == "persistent-payload" ]] && ok "data_after_repair" || fail "data_after_repair"

# ------------------------------------------------------------------- #
step 13 "ломаем копию SB, перемонтируем, основной должен по-прежнему работать"
umount "$MNT"
dd if=/dev/zero of="$LOOP" bs=512 count=1 seek=$SB2 conv=notrunc status=none
mount -t bnkfs "$LOOP" "$MNT" 2>&1
mag1=$(xxd -l 4 -s $((SB1 * 512)) "$LOOP" | awk '{print $2$3}')
[[ "$mag1" == "464b4e42" ]] && ok "mount_with_broken_backup" || fail "mount_with_broken_backup"

# ------------------------------------------------------------------- #
step 14 "ioctl erase: обе копии SB должны обнулиться, следующий mount форматирует"
./user/bnkfs_cli "$MNT" erase
umount "$MNT"
m1=$(xxd -l 4 -s $((SB1 * 512)) "$LOOP" | awk '{print $2$3}')
m2=$(xxd -l 4 -s $((SB2 * 512)) "$LOOP" | awk '{print $2$3}')
echo "primary after erase : $m1"
echo "backup  after erase : $m2"
dmesg -c >/dev/null 2>&1 || true
mount -t bnkfs "$LOOP" "$MNT" 2>&1
dmesg | tail -3 | grep -q "форматирую" && fmt=1 || fmt=0
re1=$(xxd -l 4 -s $((SB1 * 512)) "$LOOP" | awk '{print $2$3}')
re2=$(xxd -l 4 -s $((SB2 * 512)) "$LOOP" | awk '{print $2$3}')
echo "primary after remount: $re1"
echo "backup  after remount: $re2"
[[ "$m1" == "00000000" && "$m2" == "00000000" \
   && "$re1" == "464b4e42" && "$re2" == "464b4e42" && "$fmt" -eq 1 ]] \
    && ok "erase" || fail "erase"

# ------------------------------------------------------------------- #
step 15 "валидация параметров: sb1==sb2 должно отклоняться"
umount "$MNT"
rmmod bnkfs
insmod kernel/bnkfs.ko disk_name="$LOOPNAME" \
       sb1_offset=0 sb2_offset=0 \
       max_name_len=$NAME_LEN max_file_sectors=$M_SECTORS 2>&1
rc=0
mount -t bnkfs "$LOOP" "$MNT" 2>/dev/null || rc=$?
dmesg | tail -1
echo "mount rc: $rc"
[[ "$rc" -ne 0 ]] && ok "param_same_sb" || fail "param_same_sb"
rmmod bnkfs

# ------------------------------------------------------------------- #
echo
echo "================================================================"
echo "SUMMARY: $PASS passed, $FAIL failed"
if (( FAIL )); then
    echo "Failed steps: ${FAILED_STEPS[*]}"
    exit 1
fi
echo "ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ"
