/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/rational.h (nextbsd-kernel-extensions-graphics#51).
 *
 * One symbol: vc4_hdmi.c uses it to find the best rational approximation of a
 * pixel clock ratio within given bounds, when programming HDMI clock dividers.
 *
 * This is a real algorithm, not a stub -- getting it wrong yields a wrong
 * pixel clock and no picture. Continued-fraction expansion, the same method
 * Linux's lib/rational.c uses.
 */
#ifndef _LINUXKPI_LINUX_RATIONAL_H_
#define	_LINUXKPI_LINUX_RATIONAL_H_

#include <linux/types.h>

/*
 * Find the closest n/d to given_numerator/given_denominator with
 * n <= max_numerator and d <= max_denominator.
 *
 * Semiconvergents of the continued fraction expansion: each step takes the
 * integer part, then recurses on the remainder, keeping the previous two
 * convergents. When a bound is exceeded, back off along the last term.
 */
static inline void
rational_best_approximation(unsigned long given_numerator,
    unsigned long given_denominator, unsigned long max_numerator,
    unsigned long max_denominator, unsigned long *best_numerator,
    unsigned long *best_denominator)
{
	unsigned long n, d, n0, d0, n1, d1, a, t;

	n = given_numerator;
	d = given_denominator;
	n0 = d1 = 0;
	n1 = d0 = 1;

	for (;;) {
		if (d == 0)
			break;
		t = d;
		a = n / d;
		d = n % d;
		n = t;

		t = n0 + a * n1;
		if (t > max_numerator)
			break;
		n0 = n1; n1 = t;

		t = d0 + a * d1;
		if (t > max_denominator)
			break;
		d0 = d1; d1 = t;
	}

	*best_numerator = n1;
	*best_denominator = d1;
}

#endif /* _LINUXKPI_LINUX_RATIONAL_H_ */
