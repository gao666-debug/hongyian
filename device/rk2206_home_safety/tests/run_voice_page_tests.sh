#!/usr/bin/env bash
# 语音帧解析测试：不需要 SDK、不需要板子。
set -eu
p=$(cd "$(dirname "$0")/.." && pwd)
b=$(mktemp -d /tmp/voice-page-tests.XXXXXX)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
for profile in 0 1; do
 echo "--- VOICE_DEDICATED_IDX=$profile ---"
 cc -std=c99 -Wall -Wextra -Wno-unused-parameter \
    -DSAFETY_HOST_TEST -DVOICE_DEDICATED_IDX="$profile" \
    -I"$p/tests/stubs" -I"$p/include" \
    "$p/tests/test_voice_pages.c" "$p/src/su_03t.c" -lm -o "$b/test"
 "$b/test"
done
