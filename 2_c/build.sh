#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -O2 -Wall -Wextra}"
LDFLAGS="${LDFLAGS:-}"

cd "$ROOT_DIR"

$CC $CFLAGS gateway.c smartcity_common.c -o gateway $LDFLAGS -pthread
$CC $CFLAGS sensor_temperature.c smartcity_common.c -o sensor_temperature $LDFLAGS -pthread
$CC $CFLAGS sensor_air_quality.c smartcity_common.c -o sensor_air_quality $LDFLAGS -pthread
$CC $CFLAGS actuator_camera.c smartcity_common.c -o actuator_camera $LDFLAGS -pthread
$CC $CFLAGS actuator_traffic_light.c smartcity_common.c -o actuator_traffic_light $LDFLAGS -pthread
$CC $CFLAGS actuator_street_light.c smartcity_common.c -o actuator_street_light $LDFLAGS -pthread
$CC $CFLAGS client.c smartcity_common.c -o client $LDFLAGS -pthread

echo "Build concluido."
