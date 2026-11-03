#!/usr/bin/env bash
set -eu
project_dir=$(cd "$(dirname "$0")/.." && pwd)
repo_dir=$(cd "$project_dir/../../../../.." && pwd)
build_dir=$(mktemp -d /tmp/living-tests.XXXXXX)
trap 'find "$build_dir" -type f -delete; rmdir "$build_dir"' EXIT
cc -std=c99 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all -I"$project_dir/tests/stubs" -I"$project_dir/include" -I"$repo_dir/third_party/cJSON" "$project_dir/src/living.c" "$project_dir/src/monitor_logic.c" "$project_dir/tests/test_living.c" "$repo_dir/third_party/cJSON/cJSON.c" -lm -o "$build_dir/test_living"
"$build_dir/test_living"

cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined -I"$project_dir/tests/stubs" -I"$project_dir/include" "$project_dir/tests/test_key.c" -o "$build_dir/test_key"
"$build_dir/test_key"
