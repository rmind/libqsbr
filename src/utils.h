/*
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Berkeley Software Design, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)cdefs.h	8.8 (Berkeley) 1/9/95
 */

/*
 * Various helper macros.  Portions are taken from NetBSD's sys/cdefs.h
 * and sys/param.h headers.
 */

#ifndef	_UTILS_H_
#define	_UTILS_H_

#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>

/*
 * A regular assert (debug/diagnostic only).
 */
#if !defined(ASSERT)
#define	ASSERT		assert
#endif

/*
 * Branch prediction macros.
 */

#ifndef __predict_true
#define	__predict_true(x)	__builtin_expect((x) != 0, 1)
#define	__predict_false(x)	__builtin_expect((x) != 0, 0)
#endif

#ifndef __unused
#define	__unused		__attribute__((__unused__))
#endif

/*
 * Compile-time assertion: if C11 static_assert() is not available,
 * then emulate it.
 */
#ifndef static_assert
#ifndef CTASSERT
#define	CTASSERT(x)		__CTASSERT99(x, __INCLUDE_LEVEL__, __LINE__)
#define	__CTASSERT99(x, a, b)	__CTASSERT0(x, __CONCAT(__ctassert,a), \
					       __CONCAT(_,b))
#define	__CTASSERT0(x, y, z)	__CTASSERT1(x, y, z)
#define	__CTASSERT1(x, y, z)	typedef char y ## z[(x) ? 1 : -1] __unused
#endif
#define	static_assert(exp, msg)	CTASSERT(exp)
#endif

/*
 * Atomic operations and memory barriers.  If C11 API is not available,
 * then wrap the GCC builtin routines.
 */

#ifndef atomic_compare_exchange_weak
#define	atomic_compare_exchange_weak(ptr, expected, desired) \
    __sync_bool_compare_and_swap(ptr, expected, desired)
#endif

#ifndef atomic_exchange
static inline void *
atomic_exchange(volatile void *ptr, void *nptr)
{
	volatile void * volatile old;

	do {
		old = *(volatile void * volatile *)ptr;
	} while (!atomic_compare_exchange_weak(
	    (volatile void * volatile *)ptr, old, nptr));

	return (void *)(uintptr_t)old; // workaround for gcc warnings
}
#endif

#ifndef atomic_fetch_add
#define	atomic_fetch_add(x,a)	__sync_fetch_and_add(x, a)
#endif

#ifndef atomic_thread_fence
#define	memory_order_acquire	__ATOMIC_ACQUIRE	// load barrier
#define	memory_order_release	__ATOMIC_RELEASE	// store barrier
#define	memory_order_acq_rel	__ATOMIC_ACQ_REL
#define	atomic_thread_fence(m)	__atomic_thread_fence(m)
#endif

#endif
