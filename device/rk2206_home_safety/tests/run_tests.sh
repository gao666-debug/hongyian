#!/usr/bin/env bash
set -eu
ulimit -c 0
project_dir=$(cd "$(dirname "$0")/.." && pwd)
repo_dir=$(cd "$project_dir/../../../../.." && pwd)
build_dir=$(mktemp -d /tmp/home-safety-tests.XXXXXX)
trap 'find "$build_dir" -type f -delete; rmdir "$build_dir"' EXIT
cc -DSAFETY_HOST_TEST -DVOICE_DEDICATED_IDX="${SAFETY_TEST_VOICE_PROFILE:-0}" -DSU03T_MEDICINE_PROFILE_ENABLED="${SAFETY_TEST_MEDICINE_PROFILE:-0}" -D_DEFAULT_SOURCE -std=c99 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-misleading-indentation -fsanitize=undefined -fno-sanitize-recover=all -pthread \
 -I"$project_dir/tests/stubs" -I"$project_dir/include" -I"$repo_dir/third_party/cJSON" \
 -I"$repo_dir/base/iot_hardware/peripheral/interfaces/kits" \
 -I"$repo_dir/third_party/paho_mqtt/MQTTClient-C/src" -I"$repo_dir/third_party/paho_mqtt/MQTTPacket/src" \
 "$project_dir/tests/test_safety.c" "$project_dir/src/living.c" "$project_dir/src/cloud_commands.c" "$project_dir/src/safety_runtime.c" "$project_dir/src/safety_logic.c" \
 "$project_dir/src/sht30.c" "$project_dir/src/mpu6050.c" "$project_dir/src/bh1750.c" "$project_dir/src/su_03t.c" "$project_dir/src/ui.c" "$project_dir/src/safety_stats.c" "$project_dir/src/event_log.c" \
 "$repo_dir/third_party/cJSON/cJSON.c" "$repo_dir/third_party/paho_mqtt/MQTTClient-C/src/MQTTClient.c" \
 "$repo_dir/third_party/paho_mqtt/MQTTClient-C/src/liteOS/MQTTLiteOS.c" \
 "$repo_dir"/third_party/paho_mqtt/MQTTPacket/src/*.c -lm -o "$build_dir/test_safety"
"$build_dir/test_safety"

cc -std=c99 -Wall -Wextra -Werror -I"$project_dir/include" "$project_dir/tests/test_voice_policy.c" -o "$build_dir/test_voice_policy"
"$build_dir/test_voice_policy"
