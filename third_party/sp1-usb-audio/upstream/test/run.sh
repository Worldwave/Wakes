#!/bin/sh
# Host tests for the packet regulator (uacring.c), at both producer block sizes
# the regulator is tuned for. Needs only a C compiler.
set -eu
D="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
for blk in 128 256; do
  cc -std=c99 -O2 -Wall -DAUDIO_BLK_FRAMES=$blk -I"$D/usb-audio" \
     "$D/test/test_uacring.c" "$D/usb-audio/uacring.c" -o "$T/test_uacring$blk"
  echo "== test_uacring, $blk-frame blocks"
  "$T/test_uacring$blk"
done
