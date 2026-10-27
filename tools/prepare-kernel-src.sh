#!/bin/sh
# prepare-kernel-src.sh — lay down the NextBSD kernel this repo's kexts build
# against: its obj tree in /usr/obj, and /usr/src prepared from the SAME
# nextbsd-kernel commit (nextbsd-kernel#325).
#
# The obj tree is kernel-obj-<target>.tar.gz from nextbsd-kernel's `continuous`
# release. The source is prepared by that commit's own ci/prepare-src.sh, the
# script the kernel build itself runs: patches/series, identity, sysent, config,
# src-overlay/ and its wiring, and the MIG servers. Before this, the jobs cloned
# nextbsd-kernel HEAD and re-applied only the patch series and config/NEXTBSD,
# so buildkernel's config(8) run regenerated the option headers without
# anything the overlay adds (COMPAT_MACH, COMPAT_LINUX, ...), and HEAD could be
# ahead of the obj tree.
#
# Which commit: the one `continuous` was built from (its targetCommitish), as
# that is the obj tree we actually have. On a kernel-updated dispatch it equals
# client_payload.sha; if a later kernel publish has replaced `continuous` since,
# the obj tree is the newer one, so we follow it and say so.
#
# usage: prepare-kernel-src.sh <target> <target_arch>
# env:   GH_TOKEN (public reads), DISPATCHED_SHA (optional, client_payload.sha)
# Exports KERNEL_COMMIT to $GITHUB_ENV when set.
set -eu

T=${1:?usage: prepare-kernel-src.sh <target> <target_arch>}
TA=${2:?usage: prepare-kernel-src.sh <target> <target_arch>}
KREPO=nextbsd/nextbsd-kernel

release_commit() {
  gh release view continuous --repo "$KREPO" --json targetCommitish --jq .targetCommitish
}

K=$(release_commit)
[ -n "$K" ] || { echo "FAIL: no continuous release on $KREPO" >&2; exit 1; }
mkdir -p /usr/obj
gh release download continuous --repo "$KREPO" \
  --pattern "kernel-obj-${T}.tar.gz" --output /tmp/kernel-obj.tar.gz --clobber
# The assets are replaced in place on each kernel publish; if one landed while
# we downloaded, we can't tell which commit the obj tree is from.
[ "$(release_commit)" = "$K" ] \
  || { echo "FAIL: $KREPO continuous moved during the download; rerun" >&2; exit 1; }
tar -C /usr/obj -xzf /tmp/kernel-obj.tar.gz

if [ -n "${DISPATCHED_SHA:-}" ] && [ "$DISPATCHED_SHA" != "$K" ]; then
  echo "::warning::kernel-updated carried $DISPATCHED_SHA, but continuous is now $K; building against $K (the obj tree we have)"
fi
echo "==> kernel obj + source from $KREPO@$K ($T)"

rm -rf /tmp/nk /tmp/nu
git clone --quiet --filter=blob:none "https://github.com/$KREPO.git" /tmp/nk
git -C /tmp/nk checkout --quiet "$K"
[ -x /tmp/nk/ci/prepare-src.sh ] \
  || { echo "FAIL: $KREPO@$K has no ci/prepare-src.sh (nextbsd-kernel#325)" >&2; exit 1; }
# migcom + the MIG wire types, as the kernel build uses (nextbsd-userland main).
git clone --quiet --depth 1 https://github.com/nextbsd/nextbsd-userland.git /tmp/nu
/tmp/nk/ci/prepare-src.sh "$T" "$TA" /tmp/nu /usr/src

[ -n "${GITHUB_ENV:-}" ] && echo "KERNEL_COMMIT=$K" >> "$GITHUB_ENV"
exit 0
