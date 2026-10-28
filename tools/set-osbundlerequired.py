#!/usr/bin/env python3
"""set-osbundlerequired.py — tag kexts with OSBundleRequired by role.

usage: set-osbundlerequired.py <table.tsv> <dir> [<dir> ...]

Sets OSBundleRequired in the Info.plist of every *.kext under each <dir> from
<table.tsv> (kext name, tab, value; # comments), and removes it from every kext
the table doesn't list, so a kext carries the key only when the table says so.

The values follow how Mac OS X 10.9 tags its own kexts (nextbsd-kernel-
extensions#4): display drivers and the graphics libraries they load with are
"Safe Boot", 3D-only accelerators carry no key. Nothing reads the key yet; it is
for a pre-root load path such as the boot loader preloading kexts.

A kext the table lists must exist under some <dir> unless the table marks it as
per-arch with a trailing "?" on its name (e.g. IntelGraphics? on arm64, which
has no Intel GPU kext).
"""
import os
import plistlib
import sys

KEY = "OSBundleRequired"


def kext_plists(root):
    for dirpath, dirnames, _ in os.walk(root):
        if dirpath.endswith(".kext"):
            p = os.path.join(dirpath, "Contents", "Info.plist")
            if os.path.isfile(p):
                yield p


def read_table(path):
    table, optional = {}, set()
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("kext\t"):
                continue
            name, value = line.split("\t")
            if name.endswith("?"):
                name = name[:-1]
                optional.add(name)
            table[name] = value
    return table, optional


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2])
    table, optional = read_table(argv[1])
    seen = set()
    for root in argv[2:]:
        n = 0
        for p in kext_plists(root):
            with open(p, "rb") as f:
                info = plistlib.load(f)
            name = info.get("CFBundleName") or os.path.basename(
                os.path.dirname(os.path.dirname(p)))[:-len(".kext")]
            if name in table:
                info[KEY] = table[name]
                seen.add(name)
            else:
                info.pop(KEY, None)
            with open(p, "wb") as f:
                plistlib.dump(info, f)
            n += 1
            print(f"  {name}: {info.get(KEY, '(none)')}")
        if n == 0:
            sys.exit(f"FAIL: no kexts under {root}")
    missing = sorted(set(table) - seen - optional)
    if missing:
        sys.exit(f"FAIL: the table names kexts that were not built: {', '.join(missing)}")


if __name__ == "__main__":
    main(sys.argv)
