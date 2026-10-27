#!/bin/sh
# gen-i915-personalities.sh — emit an Apple IOKitPersonalities block for the
# Intel i915 (i915kms) DRM/KMS graphics driver, derived from the driver's own
# PCI id table.
#
# Graphics kext series (IntelGraphics.kext <- i915kms, drm-kmod). Companion to
# gen-amdgpu/gen-radeon: Apple kexts carry IOKitPersonalities in
# Info.plist whose IOPCIPrimaryMatch device-id list the in-kernel IOService
# matcher reads to bind a driver to a device. We produce the faithful equivalent
# for i915kms from the drm-kmod source being compiled.
#
# 6.12 CHANGED THE HEADER (drm-kmod #337). Through 6.6, i915_pciids.h held flat
# INTEL_VGA_DEVICE(0x...., info) rows and a plain
#     grep INTEL_VGA_DEVICE\(0x....
# captured the whole match table. On 6.12 (include/drm/intel/i915_pciids.h) the
# ids moved into MACRO__(0x…) entries inside per-platform INTEL_<PLAT>_IDS(…)
# macros, so that grep matches ZERO rows. A naive header scrape is also wrong two
# ways: it OVER-claims xe-only platforms (INTEL_LNL_IDS / INTEL_BMG_IDS — Lunar
# Lake / Battlemage, no i915 driver in 6.12-lts) and UNDER-claims transitive
# nesting (INTEL_MTL_IDS pulls in INTEL_ARL_IDS, so Arrow Lake binds via Meteor
# Lake and must stay).
#
# The authoritative set is the driver's own pciidlist[] in i915_pci.c, which
# references the INTEL_<PLAT>_IDS macros. So we let the C preprocessor expand
# exactly those rows: #include the header (which defines every INTEL_<PLAT>_IDS,
# nesting and all), redefine the INTEL_VGA_DEVICE callback to emit a tagged id,
# replay i915_pci.c's INTEL_<PLAT>_IDS(INTEL_VGA_DEVICE, …) references, and
# scrape the tags. This resolves all nesting for free, auto-excludes anything not
# in pciidlist, and tracks future drm-kmod bumps. (Verified: 364 unique ids on
# 6.12-lts — LNL/BMG absent, ARL present.)
#
# Every entry is Intel (0x8086). IOPCIPrimaryMatch wants 0xDDDDVVVV (device high,
# vendor low), e.g. device 0x5916 -> "0x59168086".
#
# Output (stdout) is a complete `<key>IOKitPersonalities</key><dict>...</dict>`
# block ready to drop into Info.plist via `ko2kext.sh -p`.
set -eu

usage() {
	echo "usage: gen-i915-personalities.sh <i915_pciids.h> <i915_pci.c> <bundle-id> <ioclass>" >&2
	echo "  <i915_pciids.h>  include/drm/intel/i915_pciids.h (defines INTEL_<PLAT>_IDS macros)" >&2
	echo "  <i915_pci.c>     drivers/gpu/drm/i915/i915_pci.c  (holds pciidlist[]'s macro refs)" >&2
	exit 1
}

[ $# -eq 4 ] || usage
SRC="$1"; PCI_C="$2"; BUNDLE_ID="$3"; IOCLASS="$4"
[ -f "$SRC" ]   || { echo "gen-i915-personalities: no such file: $SRC" >&2; exit 1; }
[ -f "$PCI_C" ] || { echo "gen-i915-personalities: no such file: $PCI_C" >&2; exit 1; }

VENDOR=8086   # PCI_VENDOR_ID_INTEL — every INTEL_VGA_DEVICE entry is Intel.

# The pciidlist[] macro references in i915_pci.c — the exact platform id sets the
# driver binds. Strip line comments so they can't smuggle stray text.
REFS=$(grep -E 'INTEL_[A-Z0-9_]+_IDS[[:space:]]*\(INTEL_VGA_DEVICE' "$PCI_C" | sed -E 's@/\*.*@@')
[ -n "$REFS" ] || { echo "gen-i915-personalities: no INTEL_<PLAT>_IDS(INTEL_VGA_DEVICE refs in $(basename "$PCI_C")" >&2; exit 1; }

# Let the preprocessor expand exactly those rows. The header is self-contained
# (no kernel includes, so no -I needed). INTEL_VGA_DEVICE is redefined to emit
# @@<id>@@; the discarded info arg may name undefined symbols — harmless under -E.
IDS=$(
	{
		printf '#include "%s"\n' "$SRC"
		printf '#undef INTEL_VGA_DEVICE\n'
		printf '#define INTEL_VGA_DEVICE(id, info) @@id@@\n'
		printf 'int nbkm_i915_ids[] = {\n%s\n};\n' "$REFS"
	} | "${CC:-cc}" -E -P -xc - 2>/dev/null \
		| grep -oE '@@0x[0-9A-Fa-f]+@@' \
		| grep -oE '0x[0-9A-Fa-f]+' \
		| tr 'A-F' 'a-f' \
		| sort -u
)
[ -n "$IDS" ] || { echo "gen-i915-personalities: pciidlist expansion produced no ids (compiler/header issue?)" >&2; exit 1; }

# Build the space-separated IOPCIPrimaryMatch string: 0x<device><vendor>.
MATCH=""
N=0
for id in $IDS; do
	dev=${id#0x}
	while [ ${#dev} -lt 4 ]; do dev="0${dev}"; done
	MATCH="${MATCH}0x${dev}${VENDOR} "
	N=$((N + 1))
done
MATCH=${MATCH% }   # trim trailing space

# IOProbeScore: higher than the generic VGA/framebuffer nexus so the real KMS
# driver wins the match. Mirrors Apple kexts (and the sibling generators).
cat <<EOF
	<key>IOKitPersonalities</key>
	<dict>
		<key>${IOCLASS}</key>
		<dict>
			<key>CFBundleIdentifier</key>
			<string>${BUNDLE_ID}</string>
			<key>IOClass</key>
			<string>${IOCLASS}</string>
			<key>IOProviderClass</key>
			<string>IOPCIDevice</string>
			<key>IOPCIPrimaryMatch</key>
			<string>${MATCH}</string>
			<key>IOProbeScore</key>
			<integer>10000</integer>
		</dict>
	</dict>
EOF

echo "gen-i915-personalities: ${N} Intel i915 device ids from $(basename "$PCI_C") pciidlist (preprocessor-expanded)" >&2
