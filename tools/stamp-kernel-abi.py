#!/usr/bin/env python3
"""stamp-kernel-abi.py — record which kernel ABI every kext was built for.

usage: stamp-kernel-abi.py <KERNEL-ABI-VERSION> <KERNEL-BUILD-ID> <dir> [<dir> ...]

Sets two keys in the Info.plist of every *.kext under each <dir> (including
kexts nested in PlugIns), from the kernel SDK the kexts were compiled against
(nextbsd-kernel#376):

  NBSDKernelABIVersion  the SDK's KERNEL-ABI-VERSION, the kernel version's
                        major.minor (e.g. "27.0"). OSKext checks it against the
                        running kernel's kern.osrelease, so a kernel release
                        that keeps the ABI needs no kext rebuild. An SDK from
                        before #376 has none; fetch-kernel-sdk.sh then passes
                        its KERNEL-BUILD-ID here instead.
  NBSDKernelBuildID     the legacy key (nextbsd-kernel#326): the SDK's
                        KERNEL-BUILD-ID, whatever it holds. The pinned
                        nextbsd-ci and nextbsd-pkg compare it with the
                        kernel's KERNEL-BUILD-ID by string equality, so it is
                        written unchanged.

The keys live in each bundle rather than in a file beside the bundles because
the kext tarballs are unpacked straight into /System/Library/Extensions: a
loose file there would be installed, and one owned by several packages is a
pkg file conflict. In the plist they travel with the kext into the image,
where packaging, the image lanes and OSKext can each compare them with the
kernel.

Fails if a kext already carries a different value for either key, or if a
<dir> holds no kexts.
"""
import os
import plistlib
import sys

ABI_KEY = "NBSDKernelABIVersion"
ID_KEY = "NBSDKernelBuildID"


def kext_plists(root):
    for dirpath, dirnames, _ in os.walk(root):
        if dirpath.endswith(".kext"):
            p = os.path.join(dirpath, "Contents", "Info.plist")
            if os.path.isfile(p):
                yield p


def main(argv):
    if len(argv) < 4 or not argv[1].strip() or not argv[2].strip():
        sys.exit(__doc__.strip().splitlines()[2])
    want = {ABI_KEY: argv[1].strip(), ID_KEY: argv[2].strip()}
    total = 0
    for root in argv[3:]:
        n = 0
        for p in kext_plists(root):
            with open(p, "rb") as f:
                info = plistlib.load(f)
            for key, value in want.items():
                old = info.get(key)
                if old is not None and old != value:
                    sys.exit(f"FAIL: {p} already has {key} {old}, not {value}")
                info[key] = value
            with open(p, "wb") as f:
                plistlib.dump(info, f)
            n += 1
        if n == 0:
            sys.exit(f"FAIL: no kexts under {root}")
        print(f"  {root}: {n} kext(s) stamped {ABI_KEY}={want[ABI_KEY]} {ID_KEY}={want[ID_KEY]}")
        total += n
    print(f"==> {total} kext(s) stamped")


if __name__ == "__main__":
    main(sys.argv)
