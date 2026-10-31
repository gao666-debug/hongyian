#!/bin/sh
set -eu
cd "$(dirname "$0")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
cc -std=c99 -Wall -Wextra -Werror -Istubs -I../src \
  test_lcd.c ../src/lcd_bringup.c -o "$work/test-lcd"
"$work/test-lcd"
cc -std=c99 -Wall -Wextra -Werror -Istubs -I../src \
  -fsyntax-only ../src/lcd_only_main.c
