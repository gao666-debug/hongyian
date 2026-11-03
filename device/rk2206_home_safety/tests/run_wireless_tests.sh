#!/usr/bin/env bash
set -eu
p=$(cd "$(dirname "$0")/.." && pwd)
b=$(mktemp -d /tmp/wireless-tests.XXXXXX)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
cc -DSAFETY_HOST_TEST -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined -I"$p/tests/door_stubs" -I"$p/include" "$p/tests/test_wireless.c" "$p/src/button.c" -o "$b/test"
"$b/test"

cc -DSAFETY_HOST_TEST -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined -I"$p/tests/door_stubs" -I"$p/include" "$p/tests/test_wireless_door.c" "$p/src/button.c" "$p/src/door.c" -o "$b/test_coexist"
"$b/test_coexist"
