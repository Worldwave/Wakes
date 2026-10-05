#!/usr/bin/env bash
# The patch IS the change: src/udc_nrf.c must equal the pristine upstream copy
# with patches/udc_nrf-iso-in-fast.patch applied. Regenerate the patch with
#   tools/check.sh --regen
set -euo pipefail
D="$(cd "$(dirname "$0")/.." && pwd)"
UP="$D/patches/udc_nrf.c.upstream"
PATCH="$D/patches/udc_nrf-iso-in-fast.patch"
if [ "${1:-}" = "--regen" ]; then
  (cd "$D" && diff -u --label a/drivers/usb/udc/udc_nrf.c --label b/drivers/usb/udc/udc_nrf.c \
      patches/udc_nrf.c.upstream src/udc_nrf.c > "$PATCH") || true
  echo "regenerated $PATCH"
fi
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp "$UP" "$T/udc_nrf.c"
patch -s "$T/udc_nrf.c" "$PATCH"
cmp -s "$T/udc_nrf.c" "$D/src/udc_nrf.c" || { echo "!! src/udc_nrf.c differs from upstream + patch"; exit 1; }
echo "nrf-usbd-isofast: src/udc_nrf.c == upstream + patch"
