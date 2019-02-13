/*
 * Copyright (c) 2015-2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#ifndef	_UTILS_H_
#define	_UTILS_H_

#include <assert.h>

/*
 * A regular assert (debug/diagnostic only).
 */
#if defined(DEBUG)
#define	ASSERT		assert
#else
#define	ASSERT(x)
#endif

/*
 * Branch prediction macros.
 */
#ifndef __predict_true
#define	__predict_true(x)	__builtin_expect((x) != 0, 1)
#define	__predict_false(x)	__builtin_expect((x) != 0, 0)
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
atomic_exchange(volatile void *ptr, void *newval)
{
	void * volatile *ptrp = (void * volatile *)ptr;
	void *oldval;
again:
	oldval = *ptrp;
	if (!__sync_bool_compare_and_swap(ptrp, oldval, newval)) {
		goto again;
	}
	return oldval;
}
#endif

#ifndef atomic_fetch_add
#define	atomic_fetch_add(x,a)	__sync_fetch_and_add(x, a)
#endif

#ifndef atomic_thread_fence
#define	memory_order_relaxed	__ATOMIC_RELAXED
#define	memory_order_acquire	__ATOMIC_ACQUIRE
#define	memory_order_release	__ATOMIC_RELEASE
#define	memory_order_seq_cst	__ATOMIC_SEQ_CST
#define	atomic_thread_fence(m)	__atomic_thread_fence(m)
#endif
#ifndef atomic_store_explicit
#define	atomic_store_explicit	__atomic_store_n
#endif
#ifndef atomic_load_explicit
#define	atomic_load_explicit	__atomic_load_n
#endif

/*
 * Exponential back-off for the spinning paths.
 */
#define	SPINLOCK_BACKOFF_MIN	4
#define	SPINLOCK_BACKOFF_MAX	128
#if defined(__x86_64__) || defined(__i386__)
#define SPINLOCK_BACKOFF_HOOK	__asm volatile("pause" ::: "memory")
#else
#define SPINLOCK_BACKOFF_HOOK
#endif
#define	SPINLOCK_BACKOFF(count)					\
do {								\
	for (int __i = (count); __i != 0; __i--) {		\
		SPINLOCK_BACKOFF_HOOK;				\
	}							\
	if ((count) < SPINLOCK_BACKOFF_MAX)			\
		(count) += (count);				\
} while (/* CONSTCOND */ 0);

#endif
