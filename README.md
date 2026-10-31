# nextbsd-kernel-extensions-graphics

Builds NextBSD kernel extensions layered on the kernel `obj` tree, inside the
SHA-pinned [`nextbsd-toolchain`](https://github.com/nextbsd/nextbsd-toolchain)
container.

## Why a separate build?

Kernel modules are linked against the exact kernel config and source. A custom
kernel means stock `kmod` packages won't load reliably, so modules are built
from the same patched source the kernel was built from.

## Triggers

- `repository_dispatch: kernel-updated` — from `nextbsd-kernel` after a full
  (source-changing) kernel build. Carries `run_id` (to fetch the
  `kernel-obj-amd64` artifact), the pinned `image_tag`, and `fbsd_sha`.
- `workflow_dispatch` — manual.

Set `client_payload[release]=true` on the dispatch to also publish a
`tar.gz` of the modules to GitHub Releases.

## Kernel SDK and ABI version

The kexts build against the kernel SDK from nextbsd-kernel's `continuous`
release (`tools/fetch-kernel-sdk.sh`, shared with nextbsd-kernel-extensions and
-nvidia). `tools/stamp-kernel-abi.py` writes two keys into every kext's
Info.plist (nextbsd-kernel#376):

- `NBSDKernelABIVersion`: the SDK's `KERNEL-ABI-VERSION` (e.g. `27.0`, the
  kernel version's major.minor). OSKext refuses a kext whose ABI version
  differs from the running kernel's, so a kernel release that keeps the ABI
  (27.0.x) needs no kext rebuild. An SDK that predates #376 has none, and its
  `KERNEL-BUILD-ID` is used instead.
- `NBSDKernelBuildID`: the legacy key, the SDK's `KERNEL-BUILD-ID` whatever it
  holds, which packaging and the image lanes compare with the kernel's.

## drm-kmod and `__FreeBSD_version`

drm-kmod (`DRM_KMOD_REF` in the workflow) is cloned fresh and patched with
`graphics/drm-kmod-patches/*.patch`, in order. NextBSD keeps the releng/15.1
kernel API while `__FreeBSD_version` moves to 2700000 (nextbsd-kernel#376), so
any gate that would pick a FreeBSD 16 API at 2700000 is patched to its 15.1
path. The gates in `drm_v6.12.85_3` with a threshold of 1500000 or above:

| Site | Gate | 1501000 / 2700000 | Decision |
| --- | --- | --- | --- |
| `drivers/gpu/drm/i915/intel_freebsd.c:60` | `< 1600005` | 15.1 / **16** | patched (0008): 15.1 `BUS_ALLOC_RESOURCE` (`int *rid`) always |
| `drivers/gpu/drm/i915/intel_freebsd.c:81` | `< 1500015` | new / new | unchanged: 15.1's own path at both |
| `drivers/gpu/drm/drm_syncobj.c:235, 303, 358, 396, 542, 575, 1423` | `(>= 1500508 && < 1600000) \|\| >= 1600011` | true / true | unchanged: 15.1 has this API (MFC), same branch |
| `drivers/gpu/drm/drm_ioctl.c:683` (`:670` before patch 0005) | same | true / true | unchanged |
| `include/drm/drm_syncobj.h:56` | same | true / true | unchanged |
| `drivers/gpu/drm/i915/gt/intel_engine_cs.c:2067` | `(>= 1500508 && < 1600000) \|\| >= 1600008` | true / true | unchanged |
| `drivers/gpu/drm/i915/gem/i915_gem_mman.c:172` | `>= 1500038` | true / true | unchanged |
| `drivers/gpu/drm/i915/gem/i915_gem_mman.c:269` | `< 1500508 \|\| (>= 1600000 && < 1600015)` | false / false | unchanged |
| `drivers/gpu/drm/i915/gem/i915_gem_pages.c:331` | `< 1500505 \|\| (>= 1600000 && < 1600007)` | false / false | unchanged |

This repo's own sources have one gate, `graphics/linuxkpi_virtio/linux_virtio.c:410`
(`>= 1500000`), true at both. When `DRM_KMOD_REF` moves, re-run the survey:
a new `< 16000xx` gate, or a `>= 16000xx` one without a 15.1 range beside it,
needs a patch like 0008.

## Name

Modules are built here and packaged as `.kext` bundles; the assembled package
in `nextbsd-pkg` is `NextBSD-kernel-extensions`, and this repository is named to
match. It was `nextbsd-kernel-modules` until 2026-09-01; GitHub redirects the
old URL.
