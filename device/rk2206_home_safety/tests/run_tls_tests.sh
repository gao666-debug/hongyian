#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -eu
ulimit -c 0
project_dir=$(cd "$(dirname "$0")/.." && pwd)
repo_dir=$(cd "$project_dir/../../../../.." && pwd)
build_dir=$(mktemp -d /tmp/home-safety-tls.XXXXXX)
trap 'find "$build_dir" -type f -delete; rmdir "$build_dir"' EXIT
cc -DSAFETY_HOST_TEST -DMBEDTLS_ECP_RESTARTABLE -D_DEFAULT_SOURCE -std=c99 -O1 -g -pthread \
 -I"$project_dir/tests/stubs" -I"$project_dir/include" -I"$repo_dir/third_party/cJSON" \
 -I"$repo_dir/base/iot_hardware/peripheral/interfaces/kits" \
 -I"$repo_dir/third_party/paho_mqtt/MQTTClient-C/src" -I"$repo_dir/third_party/paho_mqtt/MQTTPacket/src" \
 -I"$repo_dir/third_party/mbedtls/include" -I"$repo_dir/device/rockchip/rk2206/adapter/include" \
 "$project_dir/tests/test_tls.c" "$project_dir"/src/{cloud_commands,safety_runtime,safety_logic,sht30,mpu6050,bh1750,su_03t,ui,safety_stats,event_log}.c \
 "$repo_dir/third_party/cJSON/cJSON.c" "$repo_dir/third_party/paho_mqtt/MQTTClient-C/src/MQTTClient.c" \
 "$repo_dir/third_party/paho_mqtt/MQTTClient-C/src/liteOS/MQTTLiteOS.c" \
 "$repo_dir"/third_party/paho_mqtt/MQTTPacket/src/*.c "$repo_dir"/third_party/mbedtls/library/*.c -lm -o "$build_dir/test_tls"
# With no argument this is offline. A hostname argument opts into TLS-only network checks.
"$build_dir/test_tls" "$@"
