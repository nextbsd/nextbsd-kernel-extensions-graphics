/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * kunit/test-bug.h -- no-op (nextbsd-kernel-extensions-graphics#51).
 *
 * KUnit is Linux's in-kernel unit test framework. vc4 compiles a hook into its
 * register accessors so that a unit test touching real hardware fails loudly:
 *
 *	#define HVS_READ(offset)						\
 *	({									\
 *		kunit_fail_current_test("Accessing a register in a unit test!");\
 *		readl(hvs->regs + (offset));					\
 *	})
 *
 * Outside a KUnit test the upstream implementation does nothing, and there is
 * no KUnit on FreeBSD, so nothing is lost by making that explicit. This is the
 * single largest source of noise in the probe -- 161 of 202 errors -- purely
 * because the hook sits inside macros used on every register access.
 *
 * kunit_get_current_test() reports whether a test is running; always null here.
 *
 * It returns a cast 0 rather than NULL deliberately. This header is
 * FORCE-INCLUDED (-include) and therefore processed before anything else in
 * the translation unit, so NULL may not be defined yet -- which produced 17
 * "use of undeclared identifier 'NULL'" errors. Including a header to get NULL
 * would impose an include order on every source in the module; a cast constant
 * needs nothing.
 */
#ifndef _LINUXKPI_KUNIT_TEST_BUG_H_
#define	_LINUXKPI_KUNIT_TEST_BUG_H_

#define	kunit_fail_current_test(fmt, ...)	do { } while (0)

static inline struct kunit *
kunit_get_current_test(void)
{

	return ((struct kunit *)0);
}

#endif /* _LINUXKPI_KUNIT_TEST_BUG_H_ */
