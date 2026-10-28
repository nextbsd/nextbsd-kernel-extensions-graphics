#!/bin/sh
# personalities-selftest.sh — prove the graphics-kext IOKitPersonalities
# generators (i915 / amdgpu / radeon) on every PR, without needing the full
# drm-kmod build.
#
# The drm-kmod source build isn't wired into CI yet, so we exercise the parsers
# against synthetic PCI-id fixtures shaped exactly like the real i915_pciids.h /
# amdgpu_drv.c / drm_pciids.h, and assert the emitted IOPCIPrimaryMatch table.
# Critically asserts radeon EXCLUDES r128 (also vendor 0x1002) — the macro-scope
# trap a naive whole-file 0x1002 grep would fall into.
#
# (gen-em / gen-iwlwifi moved to nextbsd-kernel-extensions with their kexts.) This
# is the equivalent proof for the graphics generators. Runs on PRs (build.yml);
# publish stays gated to main.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLS="$HERE/../tools"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

chmod +x "$TOOLS"/gen-i915-personalities.sh \
         "$TOOLS"/gen-amdgpu-personalities.sh \
         "$TOOLS"/gen-radeon-personalities.sh

# --- i915: 6.12-shape (#337). IDs live in INTEL_<PLAT>_IDS(MACRO__, info) macros,
#     not flat INTEL_VGA_DEVICE(0x..) literals, and the driver's pciidlist[] in
#     i915_pci.c is the authoritative set. The generator preprocessor-expands it.
#     This fixture proves: (a) case-normalisation (0x591E), (b) transitive nesting
#     (ARL pulled in via MTL), (c) exclusion of a header macro the pciidlist does
#     NOT reference (the xe-only over-claim trap). Needs cc (ubuntu runners have it). ---
cat > "$WORK/i915_pciids.h" <<'EOF'
#define INTEL_FOO_IDS(MACRO__, info) \
	MACRO__(0x5916, info), \
	MACRO__(0x591E, info)
#define INTEL_ARL_SUB_IDS(MACRO__, info) \
	MACRO__(0x7D41, info)
#define INTEL_MTL_IDS(MACRO__, info) \
	MACRO__(0x7D60, info), \
	INTEL_ARL_SUB_IDS(MACRO__, info)
/* xe-only platform: defined here but NOT referenced by the pciidlist below */
#define INTEL_XE_ONLY_IDS(MACRO__, info) \
	MACRO__(0x6420, info)
EOF
cat > "$WORK/i915_pci.c" <<'EOF'
static const struct pci_device_id pciidlist[] = {
	INTEL_FOO_IDS(INTEL_VGA_DEVICE, &foo_info),
	INTEL_MTL_IDS(INTEL_VGA_DEVICE, &mtl_info),
	/* INTEL_XE_ONLY_IDS intentionally not referenced (xe-only) */
	INTEL_VGA_DEVICE(0, 0)
};
EOF
if command -v cc >/dev/null 2>&1 || command -v "${CC:-}" >/dev/null 2>&1; then
	I915=$("$TOOLS/gen-i915-personalities.sh" "$WORK/i915_pciids.h" "$WORK/i915_pci.c" org.nextbsd.kext.intelgraphics IntelGraphics)
	printf '%s\n' "$I915"
	printf '%s\n' "$I915" | grep -q '0x59168086' || fail "i915: 0x5916 (FOO) missing"
	printf '%s\n' "$I915" | grep -q '0x591e8086' || fail "i915: 0x591E not lowercased"
	printf '%s\n' "$I915" | grep -q '0x7d608086' || fail "i915: 0x7D60 (MTL direct) missing"
	printf '%s\n' "$I915" | grep -q '0x7d418086' || fail "i915: 0x7D41 (ARL nested via MTL) missing — transitive expansion broken"
	if printf '%s\n' "$I915" | grep -q '0x64208086'; then
		fail "i915: 0x6420 (xe-only, not in pciidlist) wrongly included — over-claim"
	fi
	echo "i915: 6.12 pciidlist expansion OK (nesting resolved, xe-only excluded)"
else
	echo "i915: SKIP (no C preprocessor available; generator needs cc)"
	I915=""
fi

# --- amdgpu: {0x1002, 0x....} pci_device_id rows. 0x6649 is a CIK id (Bonaire)
#     that ALSO appears in the radeon fixture below — the de-overlap case. ---
cat > "$WORK/amdgpu_drv.c" <<'EOF'
static const struct pci_device_id pciidlist[] = {
	{0x1002, 0x67DF, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_POLARIS10},
	{0x1002, 0x6649, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_BONAIRE},
	{0, 0, 0}
};
EOF
AMD=$("$TOOLS/gen-amdgpu-personalities.sh" "$WORK/amdgpu_drv.c" org.nextbsd.kext.amdgraphics AMDGraphics)
printf '%s\n' "$AMD"
printf '%s\n' "$AMD" | grep -q '0x67df1002' || fail "amdgpu: 0x67DF missing"
printf '%s\n' "$AMD" | grep -q '0x66491002' || fail "amdgpu: 0x6649 (CIK Bonaire) missing"

# amdgpu's match words become radeon's de-overlap exclusion set — the way the
# build wires it: generate AMDGraphics first, feed its ids to gen-radeon -x.
printf '%s\n' "$AMD" | grep -oE '0x[0-9a-f]{8}' | sort -u > "$WORK/amdgpu-match.txt"

# --- radeon: radeon_PCI_IDS (incl. 0x6649, the CIK overlap with amdgpu) PLUS an
#     r128_PCI_IDS block (also vendor 0x1002) that MUST be excluded by macro
#     scoping. Generated WITH -x so 0x6649 is de-overlapped to amdgpu. ---
cat > "$WORK/drm_pciids.h" <<'EOF'
#define radeon_PCI_IDS \
	{0x1002, 0x3150, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_RV380|RADEON_IS_MOBILITY}, \
	{0x1002, 0x6649, PCI_ANY_ID, PCI_ANY_ID, 0, 0, CHIP_BONAIRE|RADEON_IS_MOBILITY}
#define r128_PCI_IDS \
	{0x1002, 0x5046, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0}, \
	{0, 0, 0}
EOF
RAD=$("$TOOLS/gen-radeon-personalities.sh" -x "$WORK/amdgpu-match.txt" \
	"$WORK/drm_pciids.h" org.nextbsd.kext.radeongraphics RadeonGraphics)
printf '%s\n' "$RAD"
printf '%s\n' "$RAD" | grep -q '0x31501002' || fail "radeon: 0x3150 (radeon-only) missing"
if printf '%s\n' "$RAD" | grep -q '0x66491002'; then
	fail "radeon: 0x6649 (CIK) NOT de-overlapped — amdgpu should own it"
fi
if printf '%s\n' "$RAD" | grep -q '0x50461002'; then
	fail "radeon: r128 id 0x5046 wrongly included (macro-scope leak)"
fi

# --- cross-check: AMDGraphics and RadeonGraphics match sets must be DISJOINT ---
printf '%s\n' "$RAD" | grep -oE '0x[0-9a-f]{8}' | sort -u > "$WORK/radeon-match.txt"
OVERLAP=$(comm -12 "$WORK/amdgpu-match.txt" "$WORK/radeon-match.txt")
[ -z "$OVERLAP" ] || fail "AMDGraphics and RadeonGraphics share ids: $OVERLAP"
echo "de-overlap OK: AMD/Radeon match sets disjoint"

# --- well-formedness: each fragment must parse as a plist when wrapped, and the
#     match table must be all 10-char 0xDDDDVVVV words ---
if command -v python3 >/dev/null 2>&1; then
	for pair in "IntelGraphics:$I915" "AMDGraphics:$AMD" "RadeonGraphics:$RAD"; do
		cls=${pair%%:*}
		frag=${pair#*:}
		[ -n "$frag" ] || continue   # i915 skipped when no cc — nothing to check
		{
			printf '%s' '<?xml version="1.0" encoding="UTF-8"?>'
			printf '%s' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
			printf '%s' '<plist version="1.0"><dict>'
			printf '%s' "$frag"
			printf '%s' '</dict></plist>'
		} > "$WORK/wrap.plist"
		python3 - "$WORK/wrap.plist" "$cls" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
cls = sys.argv[2]
p = d['IOKitPersonalities'][cls]
assert p['IOProviderClass'] == 'IOPCIDevice', p
assert p['IOClass'] == cls, p
ids = p['IOPCIPrimaryMatch'].split()
assert ids and all(x.startswith('0x') and len(x) == 10 for x in ids), ids[:3]
print(f"{cls}: plist OK, {len(ids)} device ids")
PY
	done
fi

echo "ALL GRAPHICS PERSONALITY SELF-TESTS PASSED"
