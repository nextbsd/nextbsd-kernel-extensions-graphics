---
name: porting-linux-drivers-to-kexts
description: Port a Linux DRM/GPU driver to NextBSD as a loadable .kext. Use when vendoring Linux kernel sources into graphics/, adding a new drm-kmod-based driver, debugging a kext that builds but will not load or bind, or diagnosing a black screen / hung X server on a DRM driver.
---

# Porting Linux drivers to NextBSD kernel extensions

Hard-won rules from porting bochs and vboxvideo. Nearly every item below cost a
CI cycle or a debugging session to learn; several were wrong on the first two
attempts. Read the whole file before starting a new driver — most of these fail
*silently* or with an error that points somewhere other than the cause.

## The three layers

A missing piece lives in exactly one of these, and picking wrong means either
carrying a patch forever or waiting on someone else:

| Layer | Repo | Holds |
|---|---|---|
| LinuxKPI | FreeBSD **base** (`sys/compat/linuxkpi`) | the Linux kernel API shims |
| drm-kmod | `freebsd/drm-kmod` | vendored Linux DRM + FreeBSD glue |
| **ours** | `nextbsd-kernel-extensions/graphics/` | whatever the two above omit |

We only control layer 3. `graphics/drm_extra_helpers/` is the bucket for
"things drm-kmod does not ship or does not compile", built once and exported so
several drivers can share it.

**Check what drm-kmod actually builds, not what it contains.** It ships files
that no Makefile compiles — `drm_format_helper.c` sat in the tree with its
`EXPORT_SYMBOL`s intact and was built by nothing, so `drm_format_conv_state_*`
existed in source and in no `.ko`. It also ships *stub* headers: its
`include/drm/drm_gem_atomic_helper.h` is 15 lines declaring one function, while
Linux's is 154 — the entire shadow-plane API was simply absent.

## Vendoring discipline

- Vendor **unmodified** from the Linux tag matching drm-kmod's branch (6.12-lts
  → `v6.12`). Do not vendor an older snapshot: an abandoned upstream PR based on
  Linux 5.5 called `drm_fbdev_generic_setup()`, which no longer exists.
- Add FreeBSD changes as `#ifdef __FreeBSD__` deltas with a comment saying
  **why**, so the file stays diffable against upstream.
- To exclude a function drm-kmod already provides, wrap it in
  `#ifndef __FreeBSD__` rather than deleting it — the diff stays readable and
  the reason stays attached.
- Watch for **shadowing**: linuxkpi's own headers win the include search. A
  supplementary `video/vga.h` shadowed FreeBSD's real one; the fix was to
  `#ifndef`-supplement the missing macros in the driver, not add a header.
- Put `-I` paths for our vendored headers **after** the linuxkpi paths, so they
  can only ever be found, never shadow.

## Getting the module to load at all

These all produce `kldload: Exec format error` (ENOEXEC) or a driver that loads
and silently never binds.

- **`.name = "drmn"`.** LinuxKPI routes PCI registration by driver *name*:
  `pdrv->isdrm = strcmp(pdrv->name, "drmn") == 0;` then
  `dc = isdrm ? devclass_create("vgapci") : devclass_find("pci")`. Any other
  name registers on the plain `pci` devclass and **never probes** — the module
  loads, sits resident, and nothing happens.
- **Use `LKPI_DRIVER_MODULE`, not `drm_module_pci_driver()`.** The latter only
  emits SYSINITs and never declares a FreeBSD module, so the `.ko` carries
  dependency metadata for a module that does not exist → ENOEXEC. It also
  cannot expand: linuxkpi's `module_driver()` takes three arguments, Linux's is
  variadic and the DRM macro passes four.
- **`SYSCTL_NODE` must be in the same TU as `module_param_named()`.** linuxkpi's
  `moduleparam.h` expands tunables wherever they are declared. Putting the node
  in the companion glue file gives a duplicate symbol at link
  (`sysctl___hw_<driver>`); putting it nowhere gives an undefined one.
- **`MODULE_DEPEND` is depth-1.** `kern_linker.c` looks up symbols in
  `file->deps[]` with `deps = 0` — transitive dependencies are **not** searched.
  Declare every module you use directly, even if a dependency already depends on
  it. This is why i915kms re-declares `linuxkpi` and `dmabuf`.
- **`EXPORT_SYMS` restricts, it does not enable.** Default `NO` strips every
  global. Set `EXPORT_SYMS=YES` on any module another module links against.

## Symbols resolve at load, not at link

A `.ko` links with undefined symbols dangling. It can compile, package, and pass
every static check while being dead on arrival.

Run `tools/check-kext-symbols.py <kernel> kexts/` — it asserts every undefined
symbol is satisfied by the kernel or a peer kext, and fails with the symbol name
instead of an errno. It exists because adding the shadow-plane helpers produced
a kext that built clean and then bricked the graphics stack at boot.

Two symbol classes are resolved per-module by the kernel linker and are
undefined in every `.ko` by construction — the checker whitelists them:
`__start_set_*`/`__stop_set_*` (linker sets) and `__this_linker_file`
(`kern_linker.c` handles it by name).

## Device memory: the fictitious range

**If your driver's buffers live in VRAM, register the aperture, or X will hang.**

TTM hands raw device-BAR pfns to `lkpi_vmf_insert_pfn_prot_locked()` for any
iomem-resident BO — which every scanout buffer becomes the moment `SETCRTC`
pins it. That function resolves the pfn with `PHYS_TO_VM_PAGE()` and then
requires a **managed, unbusied** `vm_page`. Device memory has none unless the
range was registered:

```c
vm_phys_fictitious_reg_range(vram_base, vram_base + vram_size,
    VM_MEMATTR_WRITE_COMBINING);
```

`vm_phys_fictitious_init_range()` is what supplies the page, and it explicitly
clears `VPO_UNMANAGED` and sets `busy_lock = VPB_UNBUSIED` — exactly the two
properties the KPI asserts. amdgpu, radeon and i915 all already do this via
drm-kmod's `register_fictitious_range()`; `drm_gem_vram_helper` does not exist
upstream at all, which is why the omission went unnoticed for us.

Pair it with `vm_phys_fictitious_unreg_range()` on teardown **and on every init
failure path** — a second `reg_range()` over an overlapping span returns
`EINVAL`, so a leaked registration breaks `kldunload`/`kldload` and silently
reintroduces the hang.

Symptoms when it is missing, both from the same cause:

| | manifestation |
|---|---|
| `PHYS_TO_VM_PAGE()` returns NULL | `VM_FAULT_SIGBUS`, which TTM launders into `VM_FAULT_NOPAGE`, and `linux_cdev_pager_populate()` **spins at 99% CPU** |
| returns a bad page | thread **sleeps forever** in `vm_page_busy_acquire`, state `D`, CPU frozen |

Note the two look completely different in `ps`. `R<` at 99% and `D` at 0% are
the same bug.

## Missing devres wrappers

linuxkpi usually has the primitive but not the `devm_`/`pcim_` wrapper. Build it
from the pieces plus `devm_add_action_or_reset()`:

- `devm_arch_phys_wc_add` → `arch_phys_wc_add` + devres
- `pcim_request_region` → `pci_request_region` + devres
- `pcim_iomap_range` → `pci_iomap_range` + devres

**Watch return conventions.** linuxkpi's `pci_iomap_range()` returns `NULL` on
failure; Linux's `pcim_iomap_range()` returns an `ERR_PTR`. Callers test with
`IS_ERR()`, so a naive shim sails past the check and faults on first access.
Convert with `IOMEM_ERR_PTR()`.

Other gaps seen: no `<linux/spinlock_types.h>` (`spinlock_t` is in
`spinlock.h`), no `vzalloc_node` (use `__vmalloc_node`), no `gen_pool` at all
(vendor `lib/genalloc.c`).

## Licensing

Everything vendored so far is GPL-2.0-**or-later**, and `graphics/README.md`
states that as an invariant. Check the SPDX line before vendoring:
`drm_gem_shmem_helper.c`, `tiny/simpledrm.c` and the whole `sysfb/` family are
GPL-2.0-**only**. Crossing that line is a project decision, not a technical one.

## Testing: what will fool you

- **A serial-console boot test cannot see the screen.** Our kext boot test runs
  `console=comconsole` and proved `card0` existed for weeks while the actual
  display was black. Verify the framebuffer (`screendump` + colour count), not
  just the device node.
- **A test that skips is not a test that passes.** The vboxvideo stage reported
  green while printing `SKIP: VBoxGraphics.kext not in this image` — the kext
  injection list was hardcoded and did not include it.
- **Check the CI `paths` filter.** PRs touching only `graphics/` were not built
  at all until this was fixed; earlier graphics PRs got CI incidentally because
  they also touched `build.yml`.
- **Match on the right name.** `kldstat` lists the *bundle* binary
  (`VBoxGraphics`), not the KMOD name inside it (`vboxvideo`). The bochs check
  passes only by luck, because `grep -i bochs` matches `BochsGraphics`.
- **Test the real workload before believing a synthetic one.** A minimal
  reproducer and `startx` failed *differently* here (sleep vs spin); testing
  only the reproducer would have shipped a fix while leaving a 99%-CPU spin.

## Debugging method that worked

1. **Write a minimal userland reproducer.** ~220 lines of ioctls
   (`CREATE_DUMB` → `ADDFB` → `SETCRTC` → `MAP_DUMB` → `mmap` → touch) beat
   booting a desktop: deterministic, seconds, unambiguous. Note it needs to be
   the only DRM master, or `SETCRTC` returns `EPERM` and the test silently
   passes without testing anything.
2. **Bisect the trigger by elimination.** Plain mmap passed. Two live mappings
   passed. Only after `SETCRTC` did it fail — which identified *pinning into
   VRAM* as the trigger and falsified two plausible theories.
3. **`procstat -kk <pid>`** gives the kernel stack. `ps -o stat,time` tells you
   sleep (`D`, frozen) from spin (`R<`, climbing) — different bugs.
4. **Instrument rather than infer.** Four lines printing what
   `PHYS_TO_VM_PAGE()` actually returned settled a question that two rounds of
   source reading had got wrong.
5. **State predictions so they can fail.** "If it's an array alias, a 2 GB guest
   will SIGBUS instead of hanging" — it hung, which killed the theory cheaply.

## Iteration loop

CI is ~25 min. For kernel work, build locally instead: the toolchain container
(`ghcr.io/nextbsd/nextbsd-toolchain:amd64-latest`) plus the kernel
obj from nextbsd-kernel's `continuous` release reproduces the CI build in ~5 min.
Check the kernel-obj timestamp matches your test VM's `uname -r`, or the kext
will not be KBI-compatible.
