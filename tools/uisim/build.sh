#!/usr/bin/env bash
# Build and run the host UI simulator: compiles the REAL firmware ui.c with
# LVGL v9.2's software renderer and dumps one 240x240 PPM per scenario step.
#
#   ./build.sh [output_dir]      (default /tmp/frames)
#
# Dependencies are fetched once into .deps/ (gitignored):
#   - lvgl v9.2.0   (same major.minor as firmware/main/idf_component.yml)
#   - cJSON v1.7.18 (the ESP-IDF json component is a soft fork of it)
set -euo pipefail
cd "$(dirname "$0")"

DEPS=${DEPS:-.deps}   # override to keep big deps outside the repo tree
LVGL_VER=v9.2.0
CJSON_VER=v1.7.18
OUT=${1:-/tmp/frames}
mkdir -p "$DEPS" "$OUT"

if [ ! -d "$DEPS/lvgl" ]; then
  echo "==> fetch lvgl $LVGL_VER"
  curl -sL "https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/$LVGL_VER" | tar xz -C "$DEPS"
  mv "$DEPS"/lvgl-* "$DEPS/lvgl"
fi
if [ ! -d "$DEPS/cjson" ]; then
  echo "==> fetch cJSON $CJSON_VER"
  curl -sL "https://codeload.github.com/DaveGamble/cJSON/tar.gz/refs/tags/$CJSON_VER" | tar xz -C "$DEPS"
  mv "$DEPS"/cJSON-* "$DEPS/cjson"
fi

CFLAGS="-O1 -std=gnu99 -DLV_CONF_INCLUDE_SIMPLE -I. -Ishims -I$DEPS/lvgl -I$DEPS/cjson -I../../firmware/main"

echo "==> compile lvgl ($(nproc) jobs)"
OBJDIR="$DEPS/obj-$(md5sum lv_conf.h | cut -c1-8)"   # config change = fresh objects
mkdir -p "$OBJDIR"
export DEPS OBJDIR CFLAGS
find "$DEPS/lvgl/src" -name '*.c' -print0 | xargs -0 -P"$(nproc)" -n1 bash -c '
  f="$0"
  o="$OBJDIR/$(basename "$f" .c)_$(printf %s "$f" | md5sum | cut -c1-8).o"
  [ -f "$o" ] || gcc $CFLAGS -c "$f" -o "$o"
'

echo "==> link uisim"
# shellcheck disable=SC2086
gcc $CFLAGS -o uisim \
  sim_freertos.c sim_cable.c sim_main.c \
  ../../firmware/main/ui.c \
  "$DEPS/cjson/cJSON.c" \
  "$OBJDIR"/*.o \
  -lpthread

echo "==> run -> $OUT"
# Some mounts (e.g. synced workspaces) are noexec: run the binary from the
# deps dir instead, which is always a plain local path when overridden.
RUNBIN="$DEPS/uisim-run"
cp uisim "$RUNBIN"
chmod +x "$RUNBIN"
"$RUNBIN" "$OUT"
