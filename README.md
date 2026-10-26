# nextbsd-kernel-extensions

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

## Name

Modules are built here and packaged as `.kext` bundles; the assembled package
in `nextbsd-pkg` is `NextBSD-kernel-extensions`, and this repository is named to
match. It was `nextbsd-kernel-modules` until 2026-09-01; GitHub redirects the
old URL.
