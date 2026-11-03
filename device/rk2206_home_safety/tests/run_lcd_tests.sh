#!/usr/bin/env bash
set -eu
p=$(cd "$(dirname "$0")/.." && pwd)
python3 "$p/tests/check_lcd_fonts.py"
b=$(mktemp -d /tmp/lcd-tests.XXXXXX)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
cc -std=c99 -Wall -Wextra -Wno-missing-braces -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -g -fsanitize=address,undefined -fno-sanitize-recover=all -I"$p/tests/lcd_stubs" -I"$p/tests/stubs" -I"$p/include" "$p/src/lcd.c" "$p/tests/test_lcd_driver.c" -o "$b/test"
ASAN_OPTIONS=detect_leaks=0 "$b/test"
