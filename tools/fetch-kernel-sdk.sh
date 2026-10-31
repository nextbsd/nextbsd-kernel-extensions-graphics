#!/bin/sh
# fetch-kernel-sdk.sh — lay down the kernel SDK this repo's kexts build against
# (nextbsd-kernel#327, nextbsd-kernel-extensions#2).
#
# The SDK is kernel-sdk-<target>.tar.gz from nextbsd-kernel's `continuous`
# release: the kernel's headers, its generated options (opt_*.h, *_if.h, ...)
# and exported symbols, its KERNEL-BUILD-ID and, from nextbsd-kernel#376 on,
# its KERNEL-VERSION (27.0.0) and KERNEL-ABI-VERSION (27.0, the kext ABI
# version). Module source still comes from the toolchain image's freebsd-src
# (/usr/src); nothing here needs the kernel's source or obj tree, or relinks
# the kernel.
#
# usage: fetch-kernel-sdk.sh <target> [dest]
#   dest   where to unpack (default /opt/kernel-sdk); SYSDIR is <dest>/sys and
#          KERNBUILDDIR <dest>/obj
# env:   GH_TOKEN (public reads), DISPATCHED_SHA (optional, client_payload.sha)
#
# An SDK published before nextbsd-kernel#376 has no KERNEL-VERSION or
# KERNEL-ABI-VERSION; both then fall back to its KERNEL-BUILD-ID, so a kext's
# NBSDKernelABIVersion matches System.kext's exactly as NBSDKernelBuildID does.
#
# Leaves KERNEL-BUILD-ID, KERNEL-ABI-VERSION and KERNEL-VERSION in
# /tmp/kernel-build-id/, and exports KERNEL_SDK, KERNEL_COMMIT,
# KERNEL_BUILD_ID, KERNEL_ABI_VERSION and KERNEL_VERSION to $GITHUB_ENV when
# set. KERNEL_COMMIT is the nextbsd-kernel commit from the SDK's SDK-INFO.
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
[ -f "$DEST/sys/conf/kmod.mk" ] || { echo "fetch-kernel-sdk: SDK has no sys/conf/kmod.mk" >&2; exit 1; }
ls "$DEST"/obj/opt_*.h >/dev/null 2>&1 || { echo "fetch-kernel-sdk: SDK has no opt_*.h" >&2; exit 1; }

# The kernel and kext ABI versions (nextbsd-kernel#376), or the build ID.
sdkfile() { [ -s "$DEST/$1" ] && tr -d ' \t\r\n' < "$DEST/$1" || true; }
KVER=$(sdkfile KERNEL-VERSION)
KABI=$(sdkfile KERNEL-ABI-VERSION)
if [ -z "$KVER" ] || [ -z "$KABI" ]; then
	echo "::notice::kernel SDK has no KERNEL-VERSION/KERNEL-ABI-VERSION (pre nextbsd-kernel#376); using KERNEL-BUILD-ID $KID for both"
	KVER=$KID
	KABI=$KID
fi

# The kernel commit, from SDK-INFO: kernel_commit= when the SDK records it,
# else the <commit> of a <commit>.<run_id>.<run_attempt> kernel_build_id.
info() { sed -n "s/^$1=//p" "$DEST/SDK-INFO" 2>/dev/null | head -1; }
K=$(info kernel_commit)
if [ -z "$K" ]; then
	K=$(info kernel_build_id)
	case $K in
	*.*.*) K=${K%%.*} ;;
	*) K= ;;
	esac
fi
case $K in
*[!0-9a-f]* | "") K=unknown ;;
esac

if [ -n "${DISPATCHED_SHA:-}" ] && [ "$K" != unknown ] && [ "$DISPATCHED_SHA" != "$K" ]; then
	echo "::warning::kernel-updated was for $DISPATCHED_SHA, but continuous is now $K; building against $K"
fi
mkdir -p /tmp/kernel-build-id
echo "$KID" > /tmp/kernel-build-id/KERNEL-BUILD-ID
echo "$KABI" > /tmp/kernel-build-id/KERNEL-ABI-VERSION
echo "$KVER" > /tmp/kernel-build-id/KERNEL-VERSION

echo "==> kernel SDK from nextbsd/nextbsd-kernel@$K ($T): version $KVER, ABI $KABI, KERNEL-BUILD-ID $KID"
sed 's/^/    /' "$DEST/SDK-INFO"
if [ -n "${GITHUB_ENV:-}" ]; then
	echo "KERNEL_SDK=$DEST" >> "$GITHUB_ENV"
	echo "KERNEL_COMMIT=$K" >> "$GITHUB_ENV"
	echo "KERNEL_BUILD_ID=$KID" >> "$GITHUB_ENV"
	echo "KERNEL_ABI_VERSION=$KABI" >> "$GITHUB_ENV"
	echo "KERNEL_VERSION=$KVER" >> "$GITHUB_ENV"
fi
