#!/usr/bin/env bash
# Copyright (c) 2026 Ryan Gilmore
# SPDX-License-Identifier: Apache-2.0
#
# Stage the SP-1 example with pinned external hardware sources, then build it.
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${SP1_BUILD_ROOT:-$SELF/.build}"
CACHE="$WORK/cache"
VARIANT=stock
[ "${SP1_ISOFAST:-0}" = 0 ] || [ "${SP1_ISOFAST:-0}" = 1 ] || {
  echo "!! SP1_ISOFAST must be 0 or 1" >&2; exit 1; }
[ "${SP1_ISOFAST:-0}" = 0 ] || VARIANT=isofast
STAGE="$WORK/stage-$VARIANT"
BUILD="$WORK/build-$VARIANT"
OUT="$WORK/sp1_usb_audio${VARIANT/isofast/-isofast}.bin"
[ "$VARIANT" = stock ] && OUT="$WORK/sp1_usb_audio.bin"

MARISKO_COMMIT=b787d906c96db3f03b11860ec20f0b53441c266d
FELDD_COMMIT=b70a3a274b8d5c4b99d6ed1edb9f265d4588d967
MARISKO_URL=https://github.com/softmodded/marisko.git
FELDD_URL=https://github.com/bnjreece/feldd-sp1-firmware.git

[ -n "${ZEPHYR_BASE:-}" ] && [ -d "$ZEPHYR_BASE" ] || {
  echo "!! ZEPHYR_BASE must name a Zephyr source tree" >&2; exit 1; }
[ "${ZEPHYR_TOOLCHAIN_VARIANT:-}" = zephyr ] || {
  echo "!! set ZEPHYR_TOOLCHAIN_VARIANT=zephyr" >&2; exit 1; }
[ -n "${ZEPHYR_SDK_INSTALL_DIR:-}" ] && [ -d "$ZEPHYR_SDK_INSTALL_DIR" ] || {
  echo "!! ZEPHYR_SDK_INSTALL_DIR must name a Zephyr SDK" >&2; exit 1; }
command -v west >/dev/null || { echo "!! west is not on PATH" >&2; exit 1; }

mkdir -p "$CACHE"

checkout_or_clone()
{
  local env_path="$1" cache_name="$2" url="$3" commit="$4" path
  if [ -n "$env_path" ]; then
    path="$env_path"
  else
    path="$CACHE/$cache_name"
    if [ ! -d "$path/.git" ]; then
      git clone "$url" "$path"
      git -C "$path" checkout --detach "$commit"
    fi
  fi
  [ -d "$path/.git" ] || { echo "!! not a git checkout: $path" >&2; exit 1; }
  local head
  head="$(git -C "$path" rev-parse HEAD)"
  [ "$head" = "$commit" ] || {
    echo "!! $path is at $head; required pinned commit is $commit" >&2; exit 1; }
  printf '%s\n' "$path"
}

BOARD_ROOT="$(checkout_or_clone "${SP1_BOARD_ROOT:-}" marisko "$MARISKO_URL" "$MARISKO_COMMIT")"
FELDD="$(checkout_or_clone "${FELDD_DIR:-}" feldd-sp1-firmware "$FELDD_URL" "$FELDD_COMMIT")"

USBA="$SELF/../usb-audio"
MODULE="$SELF/../module/nrf-usbd-isofast"
for f in usb_audio.c usb_audio.h uacring.c uacring.h; do
  [ -f "$USBA/$f" ] || {
    echo "!! $USBA/$f is missing; run prep.sh from the assembled public tree" >&2; exit 1; }
done
for f in CMakeLists.txt Kconfig src/udc_nrf.c src/udc_nrf_iso_in_fast.h; do
  [ -f "$MODULE/$f" ] || {
    echo "!! $MODULE/$f is missing; run prep.sh from the assembled public tree" >&2; exit 1; }
done

rm -rf "$STAGE"
mkdir -p "$STAGE"
rsync -a --exclude .git "$SELF/fw/" "$STAGE/"

# External sources must never be copied into a git working tree.
if git -C "$STAGE" rev-parse --git-dir >/dev/null 2>&1; then
  echo "!! $STAGE is inside a git working tree; set SP1_BUILD_ROOT outside it" >&2
  exit 1
fi

FELDD_SRC="$FELDD/firmware/app/src"
for f in led.c led.h controls.c controls.h buttons.c buttons.h sp1_board.h; do
  [ -f "$FELDD_SRC/$f" ] || { echo "!! feldd source missing: $FELDD_SRC/$f" >&2; exit 1; }
  [ ! -e "$STAGE/src/$f" ] || {
    echo "!! fw/src/$f exists; feldd files must only be staged" >&2; exit 1; }
  cp "$FELDD_SRC/$f" "$STAGE/src/$f"
done
for f in usb_audio.c usb_audio.h uacring.c uacring.h; do
  [ ! -e "$STAGE/src/$f" ] || {
    echo "!! fw/src/$f exists; the shared usb-audio files must only be staged" >&2; exit 1; }
  cp "$USBA/$f" "$STAGE/src/$f"
done
mkdir -p "$STAGE/isofast"
cp -R "$MODULE/src" "$MODULE/CMakeLists.txt" "$MODULE/Kconfig" "$STAGE/isofast/"

EXTRA_CONF="$STAGE/txq2.conf"
if [ "$VARIANT" = isofast ]; then
  EXTRA_CONF="$EXTRA_CONF;$STAGE/isofast.conf"
fi

west build -p always --no-sysbuild -b sp1 -d "$BUILD" "$STAGE" -- \
  -DBOARD_ROOT="$BOARD_ROOT" -DCONFIG_ROM_START_OFFSET=0 \
  -DEXTRA_CONF_FILE="$EXTRA_CONF"

ELF="$BUILD/zephyr/zephyr.elf"
TOOLBIN="$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin"
[ -x "$TOOLBIN/arm-zephyr-eabi-objcopy" ] || {
  echo "!! ARM objcopy not found under the Zephyr SDK" >&2; exit 1; }
"$TOOLBIN/arm-zephyr-eabi-objcopy" -O binary "$ELF" "$OUT"

# Read symbols once: nm can otherwise receive SIGPIPE under pipefail.
SYMS="$("$TOOLBIN/arm-zephyr-eabi-nm" -C "$ELF")"
vt="$(awk '$3=="_vector_table"{print $1}' <<<"$SYMS")"
[ "$vt" = 00020000 ] || { echo "!! _vector_table at $vt, want 00020000" >&2; exit 1; }

for sym in audio_start audio_thread i2s_nrfx_configure render \
           feed_wdt wdt_ensure_started power_boot power_poll system_off gesture_step \
           audio_shutdown led_init controls_init controls_read_raw buttons_scan \
           charge_gauge_bits battery_pct vol_step vol_q8 audio_set_volume_q8 \
           audio_volume_q8 audio_hp_poll usb_audio_init usb_audio_claim \
           usb_audio_push sample_usbd_init_device usbd_enable; do
  grep -q " $sym\$" <<<"$SYMS" || { echo "!! $sym missing from image" >&2; exit 1; }
done

grep -q '^CONFIG_I2S_NRFX_TX_BLOCK_COUNT=2$' "$BUILD/zephyr/.config" || {
  echo "!! I2S TX queue is not 2 blocks" >&2; exit 1; }
grep -q '^CONFIG_USBD_AUDIO2_CLASS=y$' "$BUILD/zephyr/.config" || {
  echo "!! UAC2 class is not enabled" >&2; exit 1; }

if [ "$VARIANT" = stock ]; then
  grep -q ' usbd_uac2_send$' <<<"$SYMS" || {
    echo "!! stock image lacks usbd_uac2_send" >&2; exit 1; }
  ! grep -q ' udc_nrf_iso_in_fast_set$' <<<"$SYMS" || {
    echo "!! stock image unexpectedly contains the fast-path driver" >&2; exit 1; }
else
  grep -q ' udc_nrf_iso_in_fast_set$' <<<"$SYMS" || {
    echo "!! fast image lacks udc_nrf_iso_in_fast_set" >&2; exit 1; }
  ! grep -q ' usbd_uac2_send$' <<<"$SYMS" || {
    echo "!! fast image still links usbd_uac2_send" >&2; exit 1; }
  ! grep -q '^CONFIG_UDC_NRF=y$' "$BUILD/zephyr/.config" || {
    echo "!! fast image still enables Zephyr's stock nRF UDC" >&2; exit 1; }
fi

size="$(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT")"
echo "sp1: audits passed (vectors, watchdog, power-off, audio, UAC2, 128-frame/2-block I2S, $VARIANT path)"
echo "sp1: $size B -> $OUT"
