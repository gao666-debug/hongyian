#!/usr/bin/env bash
set -eu
p=$(cd "$(dirname "$0")/.." && pwd)
r=$(cd "$p/../../../../.." && pwd)
b=$(mktemp -d /tmp/dns-tests.XXXXXX)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
cc -D_DEFAULT_SOURCE -DSAFETY_HOST_TEST -DMQTT_DNS_FALLBACK_TEST -std=c99 -g -Wall -Wextra -Wno-sign-compare -ffunction-sections -fdata-sections -fsanitize=undefined -I"$p/tests/dns_stubs" -I"$p/tests/stubs" -I"$r/third_party/paho_mqtt/MQTTClient-C/src/liteOS" "$p/tests/test_dns_fallback.c" -Wl,--gc-sections -o "$b/test"
"$b/test"
