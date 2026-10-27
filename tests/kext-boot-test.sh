#!/bin/sh
# kext-boot-test.sh — thin entry point onto the shared nextbsd-ci boot harness
# (T4). Boots the kext image and runs the on-image GPU kext validation
# (/usr/tests/nextbsd/kext/run.sh, installed by the FreeBSD-VM mutation): kextd
# auto-loads the GPU kext on boot, so it is a plain kextstat check + the
# sentinel the harness reads. The harness exit class is the gate; the VIRTIO
# marker lives in a kext-local overlay (tests/overlay-kext.tsv), so no nextbsd-ci
# tag bump is needed. NB_BOOT_VERBOSE=1 keeps the serial transcript for diagnosis.
set -eu
IMG="${1:?usage: kext-boot-test.sh <disk.img|.img.zip|.img.gz>}"
[ -f "$IMG" ] || { echo "ERROR: $IMG not found"; exit 1; }

mkdir -p tests
case "$IMG" in
  *.zip)
    RAW=tests/disk.img
    echo "==> extracting $IMG -> $RAW"
    MEMBER=$(unzip -Z1 "$IMG" | grep -E '\.img$' | head -1)
    [ -n "$MEMBER" ] || { echo "FAIL: no .img member in $IMG" >&2; exit 1; }
    unzip -p "$IMG" "$MEMBER" > "$RAW"
    IMG=$RAW
    ;;
  *.gz)
    RAW=tests/disk.img
    echo "==> decompressing $IMG -> $RAW"
    gunzip -c "$IMG" > "$RAW"
    IMG=$RAW
    ;;
esac

echo "==> kext boot test: $IMG — shared harness, on-image GPU (virtio-gpu) kextstat"
ls -lh "$IMG"

# Fetch the shared harness at the pinned lockstep tag (absent = first run).
[ -d nextbsd-ci/.git ] || git clone --depth 1 --branch v0.3.6 \
  https://github.com/nextbsd/nextbsd-ci.git nextbsd-ci

NB_SUITE=/usr/tests/nextbsd/kext/run.sh \
NB_OVERLAY="$PWD/tests/overlay-kext.tsv" \
NB_BOOT_VERBOSE=1 NB_LOG=boot-test.log \
  sh nextbsd-ci/harness/boot-test.sh "$IMG"
