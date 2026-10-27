/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * v3d_gemfs.c stand-ins (nextbsd-kernel-extensions-graphics#66).
 *
 * Upstream mounts a private tmpfs with huge pages enabled and allocates GEM
 * objects from it, so large buffers get 2MB pages rather than 4K. The file is
 * not vendored because it is built on get_fs_type(), vfs_kern_mount() and
 * SB_KERNMOUNT, and LinuxKPI has no VFS layer to mount anything on.
 *
 * Leaving v3d->gemfs NULL is upstream's own "no huge pages" path, not a broken
 * one: drm_gem_shmem_create_with_mnt() falls back to the normal shmem
 * allocation for a NULL mount, which is what our shim in v3d_compat.h does.
 *
 * The cost is throughput on large buffers, not correctness. If it turns out to
 * matter, the FreeBSD equivalent is a superpage-backed OBJT_PHYS object rather
 * than a filesystem mount, which is a different design and not a port of this
 * file.
 */

#include "v3d_drv.h"

void
v3d_gemfs_init(struct v3d_dev *v3d)
{

	v3d->gemfs = NULL;
}

void
v3d_gemfs_fini(struct v3d_dev *v3d)
{

	v3d->gemfs = NULL;
}
