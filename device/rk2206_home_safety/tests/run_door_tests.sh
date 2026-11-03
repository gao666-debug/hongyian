#!/usr/bin/env bash
set -eu
p=$(cd "$(dirname "$0")/.." && pwd)
b=$(mktemp -d /tmp/door-tests.XXXXXX)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
for polarity in 0 1; do
 cc -std=c99 -Wall -Wextra -Werror -fsanitize=undefined -DDOOR_OPEN_LEVEL=$polarity -I"$p/tests/door_stubs" -I"$p/include" "$p/tests/test_door.c" "$p/src/door.c" -o "$b/test"
 "$b/test"
done
