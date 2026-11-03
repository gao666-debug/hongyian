#!/usr/bin/env bash
set -eu
project_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir=$(mktemp -d /tmp/monitor-tests.XXXXXX)
trap 'find "$build_dir" -type f -delete; rmdir "$build_dir"' EXIT
cc -std=c99 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
  -I"$project_dir/include" "$project_dir/src/monitor_logic.c" \
  "$project_dir/tests/test_monitor.c" -lm -o "$build_dir/test_monitor"
"$build_dir/test_monitor"
