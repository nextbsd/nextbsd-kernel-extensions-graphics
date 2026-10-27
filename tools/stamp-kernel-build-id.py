#!/usr/bin/env python3
"""stamp-kernel-build-id.py — record which kernel build every kext was built for.

usage: stamp-kernel-build-id.py <KERNEL-BUILD-ID> <dir> [<dir> ...]

Sets NBSDKernelBuildID in the Info.plist of every *.kext under each <dir>
(including kexts nested in PlugIns), to the KERNEL-BUILD-ID of the kernel-obj
the kexts were compiled against (nextbsd-kernel#326). The ID is
<commit>.<run_id>.<run_attempt> of the nextbsd-kernel build, read from
.../sys/NEXTBSD/KERNEL-BUILD-ID in that kernel's obj tree.

The ID lives in each bundle rather than in a file beside the bundles because
the kext tarballs are unpacked straight into /System/Library/Extensions: a
loose file there would be installed, and one owned by several packages is a
pkg file conflict. In the plist it travels with the kext into the image, where
packaging, the image lanes and later kextd can each compare it with the kernel.

Fails if a kext already carries a different ID, or if a <dir> holds no kexts.
"""
import os
import plistlib
import sys

KEY = "NBSDKernelBuildID"


def kext_plists(root):
    for dirpath, dirnames, _ in os.walk(root):
        if dirpath.endswith(".kext"):
            p = os.path.join(dirpath, "Contents", "Info.plist")
            if os.path.isfile(p):
                yield p


def main(argv):
    if len(argv) < 3 or not argv[1].strip():
        sys.exit(__doc__.strip().splitlines()[2])
    build_id = argv[1].strip()
    total = 0
    for root in argv[2:]:
        n = 0
        for p in kext_plists(root):
            with open(p, "rb") as f:
                info = plistlib.load(f)
            old = info.get(KEY)
            if old is not None and old != build_id:
                sys.exit(f"FAIL: {p} already has {KEY} {old}, not {build_id}")
            info[KEY] = build_id
            with open(p, "wb") as f:
                plistlib.dump(info, f)
            n += 1
        if n == 0:
            sys.exit(f"FAIL: no kexts under {root}")
        print(f"  {root}: {n} kext(s) stamped {KEY}={build_id}")
        total += n
    print(f"==> {total} kext(s) stamped")


if __name__ == "__main__":
    main(sys.argv)
