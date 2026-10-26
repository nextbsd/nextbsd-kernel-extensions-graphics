#!/bin/sh
# nvidia-activate.sh — wire one installed NVIDIA<NNN>.bundle's userland into the
# canonical /usr/X11 paths (E23 D4/D15) so X (auto-detect), ld.so, GLVND and Mesa's
# GBM loader find it. The bundle is version-locked storage that co-exists per-branch
# on disk; this makes exactly one branch "live" by symlinking its entry points into
# the standard search locations. Idempotent (ln -sf). Ships inside every bundle
# (copied by nvidia-mkbundle.sh).
#
# Single-branch systems: run once at package-build/stage time (the symlinks ship in
# NextBSD-kernel-extensions). Multi-branch later: the install/boot activator calls this
# for whichever branch autoloaded (PCI-id/kext-keyed), swapping the canonical symlinks.
#
# Where each piece goes, and which reader looks there:
#   lib/*                 -> /usr/X11/lib                  rtld (ld-elf.so.conf, D4)
#   lib/gbm/*             -> /usr/X11/lib/gbm              NextBSD-mesa's libgbm
#                                                          (-Dgbm-backends-path)
#   share/glvnd/egl_vendor.d -> /usr/X11/share/glvnd/egl_vendor.d
#                                                          NextBSD-mesa's libglvnd
#   xorg/{drivers,extensions} -> /usr/X11/lib/xorg/modules/{drivers,extensions}
#                                                          XLibre's loader (searched
#                                                          after its xlibre-25 dir)
#   share/egl/egl_external_platform.d -> /usr/share/egl/egl_external_platform.d
#       NOT /usr/X11: libEGL_nvidia (a prebuilt blob) only reads
#       /etc/egl/egl_external_platform.d:/usr/share/egl/egl_external_platform.d
#       (or $__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS). That is how it finds
#       libnvidia-egl-gbm.so.1 (EGL on GBM). No other package ships that dir.
#   share/vulkan/icd.d, etc/libmap.d -> /usr/X11/... (prefix swap only)
# Nothing goes under /usr/local, which belongs to FreeBSD packages (D4).
#
# libgbm.so.1 is not in the bundle any more: NextBSD-mesa provides it in
# /usr/X11/lib (nextbsd-kernel-extensions-nvidia#2). Mesa's libgbm and these links
# share no file path, so pkg has no file conflict to settle by deinstalling
# NextBSD-kernel-extensions (nextbsd-kernel-extensions#27).
#
# The symlink TARGETS are the bundle's absolute final install path (arg 1), so they
# resolve after install regardless of any staging prefix (arg 2). Portable POSIX sh.
# usage: nvidia-activate.sh <installed-bundle-path> [dest-root]
#   <installed-bundle-path>  final abs path, e.g. /System/Library/Extensions/NVIDIA595.bundle
#   [dest-root]              optional prefix for where the links are written
set -eu

[ $# -ge 1 ] || { echo "usage: nvidia-activate.sh <installed-bundle-path> [dest-root]" >&2; exit 1; }
BUNDLE="$1"                 # absolute FINAL location of the bundle (== symlink target base)
ROOT="${2:-}"              # optional staging prefix for where the links are created
# READ the resources from where this script actually lives (works at stage time,
# where the bundle sits under a staging prefix, and post-install alike); the symlink
# TARGET is always the final $BUNDLE path so links resolve after install.
SELF="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"   # .../<bundle>/Contents/Resources
X="$ROOT/usr/X11"

# link every file/symlink under Resources/$1 into $2 (same basenames),
# each pointing at the final $BUNDLE/Contents/Resources/$1/<name>
link_dir() {
	_src="$SELF/$1"; _dst="$2"
	[ -d "$_src" ] || return 0
	mkdir -p "$_dst"
	for _f in "$_src"/*; do
		[ -e "$_f" ] || [ -L "$_f" ] || continue
		# skip real subdirs (e.g. lib/gbm) — each has its own explicit link_dir call
		[ -d "$_f" ] && [ ! -L "$_f" ] && continue
		# Never link a libgbm: /usr/X11/lib/libgbm.so{,.1} belong to NextBSD-mesa,
		# and a second owner of that path is the file conflict pkg once settled by
		# deinstalling this whole package (nextbsd-kernel-extensions#27). Current
		# bundles carry none; this keeps an older bundle from shadowing Mesa's.
		case "$(basename "$_f")" in
		libgbm.so|libgbm.so.*) continue ;;
		esac
		ln -sf "$BUNDLE/Contents/Resources/$1/$(basename "$_f")" "$_dst/$(basename "$_f")"
	done
}

link_dir xorg/drivers                      "$X/lib/xorg/modules/drivers"
link_dir xorg/extensions                   "$X/lib/xorg/modules/extensions"
link_dir lib                               "$X/lib"
link_dir lib/gbm                           "$X/lib/gbm"
link_dir share/glvnd/egl_vendor.d          "$X/share/glvnd/egl_vendor.d"
link_dir share/egl/egl_external_platform.d "$ROOT/usr/share/egl/egl_external_platform.d"
link_dir share/vulkan/icd.d                "$X/share/vulkan/icd.d"
link_dir etc/libmap.d                      "$X/etc/libmap.d"

echo "nvidia-activate: linked $(basename "$BUNDLE") into ${X} (EGL external platforms: ${ROOT}/usr/share/egl)"
