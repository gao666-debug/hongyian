#!/usr/bin/env bash
set -eu
project_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir=$(mktemp -d /tmp/nfc-tests.XXXXXX)
trap 'find "$build_dir" -type f -delete; rmdir "$build_dir"' EXIT
cc -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -fsanitize=address,undefined -fno-sanitize-recover=all -I"$project_dir/include" -I"$project_dir/src" "$project_dir/tests/test_nfc.c" "$project_dir/src/nfc_tag.c" "$project_dir/src/rtdUri.c" "$project_dir/src/rtdText.c" "$project_dir/src/nfcForum.c" "$project_dir/src/ndef.c" -o "$build_dir/test_nfc"
"$build_dir/test_nfc"
