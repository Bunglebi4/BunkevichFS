#!/bin/sh
# test_all.sh — комплексный тест BunkevichFS внутри QEMU.
#
# Предполагается окружение:
#   - модуль bnkfs.ko лежит в /lib/modules/bnkfs.ko
#   - утилита user_tool лежит в /bin/user_tool
#   - целевое блочное устройство — /dev/vda (без таблицы разделов)
#   - /proc, /sys, /dev уже смонтированы (это делает init).
#
# Скрипт не использует bash-измы — только POSIX, чтобы работал в busybox-окружении.

set -u

DEV="${BNKFS_DEV:-/dev/vda}"
MOUNT="${BNKFS_MOUNT:-/mnt/bnkfs}"
TOOL="${BNKFS_TOOL:-/bin/user_tool}"
MODULE="${BNKFS_MODULE:-/lib/modules/bnkfs.ko}"

SB1=0
SB2=32
NAME_LEN=32
M_SECTORS=8

PASS=0
FAIL=0
TOTAL=0

# -------------------------- утилиты для логирования ------------------------- #

say()  { printf '\n=== %s ===\n' "$*"; }
ok()   { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); printf '  [ OK ] %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); printf '  [FAIL] %s\n' "$*"; }

# Запускает команду и проверяет код возврата.
expect_ok() {
	desc="$1"; shift
	if "$@" >/tmp/cmd.out 2>&1; then
		ok "$desc"
	else
		bad "$desc (rc=$?)"
		sed 's/^/        | /' /tmp/cmd.out
	fi
}

expect_fail() {
	desc="$1"; shift
	if "$@" >/tmp/cmd.out 2>&1; then
		bad "$desc (ожидали ошибку, но команда успешна)"
		sed 's/^/        | /' /tmp/cmd.out
	else
		ok "$desc (команда корректно завершилась с ошибкой)"
	fi
}

# -------------------------- начало ------------------------- #

say "Окружение"
printf 'устройство : %s\n' "$DEV"
printf 'mount-поинт: %s\n' "$MOUNT"
printf 'модуль     : %s\n' "$MODULE"
printf 'утилита    : %s\n' "$TOOL"

# Если что-то уже смонтировано — отмонтируем, чтобы тест начинался с чистого листа.
if mount | grep -q " $MOUNT "; then
	umount "$MOUNT" 2>/dev/null
fi
if lsmod 2>/dev/null | grep -q '^bnkfs'; then
	rmmod bnkfs 2>/dev/null
fi
mkdir -p "$MOUNT"

# Полностью обнуляем устройство, чтобы тест форматирования прошёл с нуля.
say "Обнуляем устройство перед стартом"
expect_ok "dd zero -> $DEV" dd if=/dev/zero of="$DEV" bs=1M count=2 conv=notrunc

# -------------------------- 1. загрузка модуля ------------------------- #

say "1. Загрузка модуля"
expect_ok "insmod bnkfs.ko" insmod "$MODULE" \
	disk_name=$(basename "$DEV") \
	sb1_offset=$SB1 sb2_offset=$SB2 \
	max_name_len=$NAME_LEN max_file_sectors=$M_SECTORS

if lsmod | grep -q '^bnkfs'; then
	ok "модуль виден в lsmod"
else
	bad "модуль не виден в lsmod"
fi

# -------------------------- 2. первый mount (форматирование) ------------------------- #

say "2. Первый mount (должно произойти форматирование)"
expect_ok "mount -t bnkfs" mount -t bnkfs "$DEV" "$MOUNT"

# В dmesg должна быть строка про форматирование.
if dmesg | tail -n 30 | grep -q "форматирую"; then
	ok "в dmesg видно сообщение о форматировании"
else
	bad "в dmesg НЕ найдено сообщение о форматировании"
	dmesg | tail -n 10 | sed 's/^/        | /'
fi

# -------------------------- 3. файлы созданы ------------------------- #

say "3. Файлы созданы и видны через VFS"
FILES=$(ls "$MOUNT" 2>/dev/null | grep '^file_' | wc -l)
if [ "$FILES" -gt 0 ]; then
	ok "обнаружено файлов: $FILES"
else
	bad "не найдено ни одного файла"
fi

# Проверка размера файла должна совпасть с M*512.
EXPECTED_SIZE=$((M_SECTORS * 512))
FIRST_FILE=$(ls "$MOUNT" | grep '^file_' | head -n 1)
ACTUAL_SIZE=$(stat -c '%s' "$MOUNT/$FIRST_FILE" 2>/dev/null || wc -c < "$MOUNT/$FIRST_FILE")
if [ "$ACTUAL_SIZE" = "$EXPECTED_SIZE" ]; then
	ok "размер $FIRST_FILE = $ACTUAL_SIZE байт (ожидали $EXPECTED_SIZE)"
else
	bad "размер $FIRST_FILE = $ACTUAL_SIZE, ожидали $EXPECTED_SIZE"
fi

# -------------------------- 4. round-trip read/write ------------------------- #

say "4. Round-trip запись и чтение всех файлов"
if "$TOOL" roundtrip "$MOUNT" >/tmp/rt.out 2>&1; then
	ok "user_tool roundtrip завершился без ошибок"
	# Считаем количество FAIL в выводе.
	N_FAIL=$(grep -c FAIL /tmp/rt.out || true)
	if [ "$N_FAIL" = "0" ]; then
		ok "ни одного FAIL в выводе roundtrip"
	else
		bad "в выводе roundtrip найдено FAIL: $N_FAIL"
	fi
else
	bad "user_tool roundtrip упал"
	sed 's/^/        | /' /tmp/rt.out
fi

# -------------------------- 5. IOCTL: список хэшей ------------------------- #

say "5. IOCTL GET_HASHES"
if "$TOOL" list "$MOUNT" >/tmp/list.out 2>&1; then
	ok "user_tool list отработал"
	# В выводе должно быть столько же записей file_, сколько файлов.
	LISTED=$(grep -c '^file_' /tmp/list.out || true)
	if [ "$LISTED" = "$FILES" ]; then
		ok "IOCTL вернул $LISTED записей (совпало с ls)"
	else
		bad "IOCTL вернул $LISTED записей, ls видит $FILES"
	fi
else
	bad "user_tool list упал"
	sed 's/^/        | /' /tmp/list.out
fi

# -------------------------- 6. IOCTL: маппинг ------------------------- #

say "6. IOCTL GET_MAPPING"
if "$TOOL" mapping "$MOUNT/$FIRST_FILE" >/tmp/map.out 2>&1; then
	ok "user_tool mapping отработал"
	cat /tmp/map.out | sed 's/^/        | /'
	# Проверим, что в выводе есть число секторов = M.
	if grep -q "всего $M_SECTORS" /tmp/map.out; then
		ok "размер маппинга совпал с M=$M_SECTORS"
	else
		bad "размер маппинга не совпал с M=$M_SECTORS"
	fi
else
	bad "user_tool mapping упал"
	sed 's/^/        | /' /tmp/map.out
fi

# Проверим обращение к несуществующему файлу.
expect_fail "mapping несуществующего файла" "$TOOL" mapping "$MOUNT/file_999999"

# -------------------------- 7. IOCTL: zero all ------------------------- #

say "7. IOCTL ZERO_ALL"
# Сначала запишем что-то ненулевое, чтобы zero реально что-то поменял.
echo "non-zero-data" > "$MOUNT/$FIRST_FILE"
sync
BEFORE=$(od -An -N 16 -tx1 "$MOUNT/$FIRST_FILE" | tr -d ' ')

expect_ok "user_tool zero" "$TOOL" zero "$MOUNT"

# Сбросим page cache, чтобы перечитать с устройства.
sync
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

AFTER=$(od -An -N 16 -tx1 "$MOUNT/$FIRST_FILE" | tr -d ' ')
ZEROS="00000000000000000000000000000000"
if [ "$AFTER" = "$ZEROS" ]; then
	ok "после ZERO_ALL файл $FIRST_FILE полон нулей"
else
	bad "после ZERO_ALL первые 16 байт: $AFTER"
fi

# -------------------------- 8. сохранение данных после umount/mount ------------------------- #

say "8. Перемонтирование сохраняет данные"
echo "persistent-payload" > "$MOUNT/$FIRST_FILE"
sync
expect_ok "umount" umount "$MOUNT"
expect_ok "повторный mount" mount -t bnkfs "$DEV" "$MOUNT"

CONTENT=$(head -c 18 "$MOUNT/$FIRST_FILE")
if [ "$CONTENT" = "persistent-payload" ]; then
	ok "содержимое файла пережило umount/mount"
else
	bad "содержимое файла потеряно: '$CONTENT'"
fi

# В dmesg на повторный mount НЕ должно быть форматирования.
if dmesg | tail -n 10 | grep -q "форматирую"; then
	bad "повторный mount зачем-то отформатировал устройство"
else
	ok "повторный mount не форматировал устройство"
fi

# -------------------------- 9. восстановление из копии SB ------------------------- #

say "9. Восстановление из резервной копии суперблока"
expect_ok "umount перед порчей" umount "$MOUNT"

# Портим первые 512 байт устройства — это основной суперблок.
expect_ok "порча основного SB" dd if=/dev/zero of="$DEV" bs=512 count=1 conv=notrunc

# Сбросим логи, чтобы поймать новые сообщения.
dmesg -c >/dev/null 2>&1 || true

expect_ok "mount после порчи основного SB" mount -t bnkfs "$DEV" "$MOUNT"

DMESG=$(dmesg | tail -n 30)
if echo "$DMESG" | grep -q "повреждён" && echo "$DMESG" | grep -q "восстанавливаю"; then
	ok "видны сообщения 'повреждён' и 'восстанавливаю' в dmesg"
else
	bad "ожидаемые сообщения о восстановлении не найдены"
	echo "$DMESG" | sed 's/^/        | /'
fi

CONTENT=$(head -c 18 "$MOUNT/$FIRST_FILE")
if [ "$CONTENT" = "persistent-payload" ]; then
	ok "данные сохранились после восстановления SB"
else
	bad "данные потеряны после восстановления SB: '$CONTENT'"
fi

# -------------------------- 10. IOCTL: erase fs ------------------------- #

say "10. IOCTL ERASE_FS"
expect_ok "user_tool erase" "$TOOL" erase "$MOUNT"
expect_ok "umount после erase" umount "$MOUNT"

dmesg -c >/dev/null 2>&1 || true
expect_ok "mount после erase (должно переформатировать)" mount -t bnkfs "$DEV" "$MOUNT"

if dmesg | tail -n 30 | grep -q "форматирую"; then
	ok "после erase mount действительно отформатировал устройство"
else
	bad "после erase ожидали форматирование, но его нет в dmesg"
	dmesg | tail -n 10 | sed 's/^/        | /'
fi

# -------------------------- финал ------------------------- #

say "Уборка"
umount "$MOUNT" 2>/dev/null
rmmod bnkfs 2>/dev/null

say "Итог"
printf 'Всего проверок: %d\n' "$TOTAL"
printf 'PASS: %d\n' "$PASS"
printf 'FAIL: %d\n' "$FAIL"

if [ "$FAIL" = "0" ]; then
	echo "=== ВСЕ ТЕСТЫ ПРОЙДЕНЫ ==="
	exit 0
else
	echo "=== ЕСТЬ ПРОВАЛЕННЫЕ ТЕСТЫ ==="
	exit 1
fi
