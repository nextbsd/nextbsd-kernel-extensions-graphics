#!/bin/sh
# fetch-kernel-sdk.sh — lay down the kernel SDK this repo's kexts build against
# (nextbsd-kernel#327, nextbsd-kernel-extensions#2).
#
# The SDK is kernel-sdk-<target>.tar.gz from nextbsd-kernel's `continuous`
# release: the kernel's headers, its generated options (opt_*.h, *_if.h, ...)
# and exported symbols, and its KERNEL-BUILD-ID. Module source still comes from
# the toolchain image's freebsd-src (/usr/src); nothing here needs the kernel's
# source or obj tree, or relinks the kernel.
#
# usage: fetch-kernel-sdk.sh <target> [dest]
#   dest   where to unpack (default /opt/kernel-sdk); SYSDIR is <dest>/sys and
#          KERNBUILDDIR <dest>/obj
# env:   GH_TOKEN (public reads), DISPATCHED_SHA (optional, client_payload.sha)
# Leaves the ID in /tmp/kernel-build-id/KERNEL-BUILD-ID, and exports
# KERNEL_SDK, KERNEL_COMMIT and KERNEL_BUILD_ID to $GITHUB_ENV when set.
set -eu

T=${1:?usage: fetch-kernel-sdk.sh <target> [dest]}
DEST=${2:-/opt/kernel-sdk}

tmp=$(mktemp -d)
gh release download continuous -R nextbsd/nextbsd-kernel \
	--pattern "kernel-sdk-$T.tar.gz" --dir "$tmp" --clobber
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
tar -C "$tmp" -xzf "$tmp/kernel-sdk-$T.tar.gz"
mv "$tmp/kernel-sdk" "$DEST"

KID=$(cat "$DEST/KERNEL-BUILD-ID")
[ -n "$KID" ] || { echo "fetch-kernel-sdk: SDK carries no KERNEL-BUILD-ID" >&2; exit 1; }
K=${KID%%.*}
[ -f "$DEST/sys/conf/kmod.mk" ] || { echo "fetch-kernel-sdk: SDK has no sys/conf/kmod.mk" >&2; exit 1; }
ls "$DEST"/obj/opt_*.h >/dev/null 2>&1 || { echo "fetch-kernel-sdk: SDK has no opt_*.h" >&2; exit 1; }

if [ -n "${DISPATCHED_SHA:-}" ] && [ "$DISPATCHED_SHA" != "$K" ]; then
	echo "::warning::kernel-updated was for $DISPATCHED_SHA, but continuous is now $K; building against $K"
fi
mkdir -p /tmp/kernel-build-id
echo "$KID" > /tmp/kernel-build-id/KERNEL-BUILD-ID

echo "==> kernel SDK from nextbsd/nextbsd-kernel@$K ($T), KERNEL-BUILD-ID $KID"
sed 's/^/    /' "$DEST/SDK-INFO"
if [ -n "${GITHUB_ENV:-}" ]; then
	echo "KERNEL_SDK=$DEST" >> "$GITHUB_ENV"
	echo "KERNEL_COMMIT=$K" >> "$GITHUB_ENV"
	echo "KERNEL_BUILD_ID=$KID" >> "$GITHUB_ENV"
fi
