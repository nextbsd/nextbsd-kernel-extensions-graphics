#!/usr/bin/env python3
"""check-kext-symbols.py — fail the build when a kext would fail kldload.

    usage: check-kext-symbols.py <kernel-elf | kernel.symbols> <kexts-dir>

The kernel side is either the kernel ELF or the kernel SDK's kernel.symbols
(one defined global or weak symbol per line, from llvm-nm -g --defined-only;
nextbsd-kernel#327).

A kernel module links with undefined symbols left dangling; they are resolved
at kldload time against the kernel and the modules already loaded. So a .ko can
build perfectly and still be dead on arrival: link_elf finds a symbol it cannot
resolve, and kldload returns ENOEXEC ("Exec format error") with the actual
symbol name buried in dmesg.

That is not hypothetical here. drm-kmod carries drm_format_helper.c in its tree,
EXPORT_SYMBOLs and all, but names it in no Makefile -- so
drm_format_conv_state_{init,copy,release} exist in source and in no .ko. Adding
the shadow-plane helpers (which embed a drm_format_conv_state) therefore
produced a drm_extra_helpers.ko that compiled clean, packaged clean, validated
clean, and then bricked the whole graphics stack at boot: IOGraphicsExtras
failed to load, and BochsGraphics went down with it because it depends on it.
Cost: one full CI cycle to find out, and the answer was only in a dmesg dump
the test had already exited before printing.

This closes that gap at build time. Every undefined symbol in every kext must be
satisfied by the kernel or by another kext in the same set. Cheap (a few hundred
ms), stdlib-only (no pyelftools on the toolchain image), and it fails with the
symbol name rather than an errno.
"""
import os
import struct
import sys

# Linker-set boundary symbols. The kernel linker synthesizes these per-module at
# load time from the linker set sections themselves; they are undefined in the
# .ko by construction and are not a missing dependency.
LINKER_SET_PREFIXES = ("__start_set_", "__stop_set_")

# Symbols the kernel linker resolves per-module rather than from any symbol
# table, so they are undefined in every .ko that references them and are not a
# missing dependency either. kern_linker.c:912 handles this one by name:
#
#     /* Treat the __this_linker_file as a special symbol. This is a
#      * global that each module refers to to get its own linker_file_t. */
#     if (strcmp(name, "__this_linker_file") == 0) { ... }
#
# Found the honest way: the first run of this checker flagged it in 7 of 9
# kexts, including ones already proven to load in the boot test.
SYNTHESIZED = frozenset(("__this_linker_file",))

STB_WEAK = 2
SHN_UNDEF = 0
SHT_SYMTAB = 2


def elf_symbols(path):
    """Return (defined_globals, undefined_strong) for an ELF64 little-endian file."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return set(), set()
    if data[4] != 2:  # ELFCLASS64 only; every target here is 64-bit.
        return set(), set()

    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)
    if e_shoff == 0 or e_shnum == 0:
        return set(), set()

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from("<I", data, off + 4)
        sh_offset, sh_size = struct.unpack_from("<QQ", data, off + 0x18)
        sh_link, = struct.unpack_from("<I", data, off + 0x28)
        sh_entsize, = struct.unpack_from("<Q", data, off + 0x38)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    defined, undefined = set(), set()
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type != SHT_SYMTAB or sh_entsize == 0:
            continue
        if sh_link >= len(sections):
            continue
        _, str_off, str_size, _, _ = sections[sh_link]
        strtab = data[str_off:str_off + str_size]
        for j in range(sh_size // sh_entsize):
            off = sh_offset + j * sh_entsize
            st_name, = struct.unpack_from("<I", data, off)
            st_info = data[off + 4]
            st_shndx, = struct.unpack_from("<H", data, off + 6)
            if st_name == 0:
                continue
            end = strtab.find(b"\0", st_name)
            name = strtab[st_name:end].decode("utf-8", "replace")
            if not name:
                continue
            bind = st_info >> 4
            if st_shndx == SHN_UNDEF:
                if bind != STB_WEAK:
                    undefined.add(name)
            elif bind != 0:  # global or weak definition
                defined.add(name)
    return defined, undefined


def main():
    if len(sys.argv) != 3:
        print(__doc__.splitlines()[2].strip(), file=sys.stderr)
        return 2
    kernel, kextdir = sys.argv[1], sys.argv[2]

    if not os.path.exists(kernel):
        print(f"check-kext-symbols: kernel not found: {kernel}", file=sys.stderr)
        return 1

    with open(kernel, "rb") as f:
        is_elf = f.read(4) == b"\x7fELF"
    if is_elf:
        provided, _ = elf_symbols(kernel)
    else:
        with open(kernel) as f:
            provided = {line.strip() for line in f if line.strip()}
    print(f"kernel {kernel}: {len(provided)} defined symbols")

    kexts = {}
    for name in sorted(os.listdir(kextdir)):
        if not name.endswith(".kext"):
            continue
        base = name[:-len(".kext")]
        binary = os.path.join(kextdir, name, "Contents", "MacOS", base)
        if os.path.exists(binary):
            kexts[base] = elf_symbols(binary)

    if not kexts:
        print(f"check-kext-symbols: no kexts found under {kextdir}", file=sys.stderr)
        return 1

    # Any kext may satisfy any other: kldload walks OSBundleLibraries, and the
    # dependency ORDER is validated separately by the Info.plist checks. What is
    # asserted here is only that the symbol exists SOMEWHERE in the shipped set.
    for defined, _ in kexts.values():
        provided |= defined

    failed = False
    for name in sorted(kexts):
        _, undefined = kexts[name]
        missing = sorted(
            s for s in undefined
            if s not in provided
            and s not in SYNTHESIZED
            and not s.startswith(LINKER_SET_PREFIXES)
        )
        if missing:
            failed = True
            print(f"\nFAIL {name}: {len(missing)} unresolved symbol(s) "
                  f"— this kext would fail kldload with ENOEXEC:")
            for s in missing:
                print(f"    {s}")
        else:
            print(f"  OK {name}: {len(undefined)} undefined, all resolvable")

    if failed:
        print("\nA symbol resolvable in no kext and not in the kernel means the "
              "defining source file is compiled into nothing. Check whether "
              "drm-kmod ships it but omits it from every Makefile — that is "
              "what happened with drm_format_helper.c.", file=sys.stderr)
        return 1

    print(f"\nAll {len(kexts)} kexts fully resolvable against kernel + peers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
