/*
 * Copyright (c) 2016-2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

/*
 * Epoch-based reclamation (EBR).  Reference:
 *
 *	K. Fraser, Practical lock-freedom,
 *	Technical Report UCAM-CL-TR-579, February 2004
 *	https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-579.pdf
 *
 * Summary:
 *
 * Any workers (threads or processes) actively referencing (accessing)
 * the globally visible objects must do that in the critical path covered
 * using the dedicated enter/exit functions.  The grace period is
 * determined using "epochs" -- implemented as a global counter (and,
 * for example, a dedicated G/C list for each epoch).  Objects in the
 * current global epoch can be staged for reclamation (garbage collection).
 * Then, the objects in the target epoch can be reclaimed after two
 * successful increments of the global epoch.  Only three epochs are
 * needed (e, e-1 and e-2), therefore we use clock arithmetics.
 *
 * See the comments in the ebr_sync() function for detailed explanation.
 */

#include <sys/queue.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "ebr.h"
#include "utils.h"

#define	ACTIVE_FLAG		(0x80000000U)

typedef struct ebr_tls {
	/*
	 * - A local epoch counter for each thread.
	 * - The epoch counter may have the "active" flag set.
	 * - Thread list entry (pointer).
	 */
	unsigned		local_epoch;
	LIST_ENTRY(ebr_tls)	entry;
} ebr_tls_t;

struct ebr {
	/*
	 * - There is a global epoch counter which can be 0, 1 or 2.
	 * - TLS with a list of the registered threads.
	 */
	unsigned		global_epoch;
	pthread_key_t		tls_key;
	pthread_mutex_t		lock;
	LIST_HEAD(, ebr_tls)	list;
};

ebr_t *
ebr_create(void)
{
	ebr_t *ebr;
	int ret;

	ret = posix_memalign((void **)&ebr, CACHE_LINE_SIZE, sizeof(ebr_t));
	if (ret != 0) {
		errno = ret;
		return NULL;
	}
	memset(ebr, 0, sizeof(ebr_t));
	if (pthread_key_create(&ebr->tls_key, free) != 0) {
		free(ebr);
		return NULL;
	}
	return ebr;
}

void
ebr_destroy(ebr_t *ebr)
{
	pthread_key_delete(ebr->tls_key);
	free(ebr);
}

/*
 * ebr_register: register the current worker (thread/process) for EBR.
 *
 * => Returns 0 on success and errno on failure.
 */
int
ebr_register(ebr_t *ebr)
{
	ebr_tls_t *t;

	t = pthread_getspecific(ebr->tls_key);
	if (__predict_false(t == NULL)) {
		int ret;

		ret = posix_memalign((void **)&t,
		    CACHE_LINE_SIZE, sizeof(ebr_tls_t));
		if (ret != 0) {
			errno = ret;
			return -1;
		}
		pthread_setspecific(ebr->tls_key, t);
	}
	memset(t, 0, sizeof(ebr_tls_t));

	pthread_mutex_lock(&ebr->lock);
	LIST_INSERT_HEAD(&ebr->list, t, entry);
	pthread_mutex_unlock(&ebr->lock);
	return 0;
}

void
ebr_unregister(ebr_t *ebr)
{
	ebr_tls_t *t;

	t = pthread_getspecific(ebr->tls_key);
	if (t == NULL) {
		return;
	}
	pthread_setspecific(ebr->tls_key, NULL);

	pthread_mutex_lock(&ebr->lock);
	LIST_REMOVE(t, entry);
	pthread_mutex_unlock(&ebr->lock);
	free(t);
}

/*
 * ebr_enter: mark the entrance to the critical path.
 */
void
ebr_enter(ebr_t *ebr)
{
	ebr_tls_t *t;
	unsigned epoch;

	t = pthread_getspecific(ebr->tls_key);
	ASSERT(t != NULL);

	/*
	 * Set the "active" flag and set the local epoch to global
	 * epoch (i.e. observe the global epoch).  Ensure that the
	 * epoch is observed before any loads in the critical path.
	 */
	epoch = ebr->global_epoch | ACTIVE_FLAG;
	atomic_store_explicit(&t->local_epoch, epoch, memory_order_relaxed);
	atomic_thread_fence(memory_order_seq_cst);
}

/*
 * ebr_exit: mark the exit of the critical path.
 */
void
ebr_exit(ebr_t *ebr)
{
	ebr_tls_t *t;

	t = pthread_getspecific(ebr->tls_key);
	ASSERT(t != NULL);

	/*
	 * Clear the "active" flag.  Must ensure that any stores in
	 * the critical path reach global visibility before that.
	 */
	ASSERT(t->local_epoch & ACTIVE_FLAG);
	atomic_thread_fence(memory_order_seq_cst);
	atomic_store_explicit(&t->local_epoch, 0, memory_order_relaxed);
}

/*
 * ebr_sync: attempt to synchronise and announce a new epoch.
 *
 * => Synchronisation points must be serialised.
 * => Return true if a new epoch was announced.
 * => Return the epoch ready for reclamation.
 */
bool
ebr_sync(ebr_t *ebr, unsigned *gc_epoch)
{
	unsigned epoch;
	ebr_tls_t *t;

	/*
	 * Ensure that any loads or stores on the writer side reach
	 * the global visibility.  We want to allow the callers to
	 * assume that the ebr_sync() call serves as a full barrier.
	 */
	epoch = atomic_load_explicit(&ebr->global_epoch, memory_order_relaxed);
	atomic_thread_fence(memory_order_seq_cst);

	/*
	 * Check whether all active workers observed the global epoch.
	 */
	LIST_FOREACH(t, &ebr->list, entry) {
		unsigned local_epoch;
		bool active;

		local_epoch = atomic_load_explicit(&t->local_epoch,
		    memory_order_relaxed);
		active = (local_epoch & ACTIVE_FLAG) != 0;

		if (active && (local_epoch != (epoch | ACTIVE_FLAG))) {
			/* No, not ready. */
			*gc_epoch = ebr_gc_epoch(ebr);
			return false;
		}
	}

	/* Yes: increment and announce a new global epoch. */
	atomic_store_explicit(&ebr->global_epoch,
	    (epoch + 1) % 3, memory_order_relaxed);

	/*
	 * Let the new global epoch be 'e'.  At this point:
	 *
	 * => Active workers: might still be running in the critical path
	 *    in the e-1 epoch or might be already entering a new critical
	 *    path and observing the new epoch e.
	 *
	 * => Inactive workers: might become active by entering a critical
	 *    path before or after the global epoch counter was incremented,
	 *    observing either e-1 or e.
	 *
	 * => Note that the active workers cannot have a stale observation
	 *    of the e-2 epoch at this point (there is no ABA problem using
	 *    the clock arithmetics).
	 *
	 * => Therefore, there can be no workers still running the critical
	 *    path in the e-2 epoch.  This is the epoch ready for G/C.
	 */
	*gc_epoch = ebr_gc_epoch(ebr);
	return true;
}

/*
 * ebr_staging_epoch: return the epoch where objects can be staged
 * for reclamation.
 */
unsigned
ebr_staging_epoch(ebr_t *ebr)
{
	/* The current epoch. */
	return ebr->global_epoch;
}

/*
 * ebr_gc_epoch: return the epoch where objects are ready to be
 * reclaimed i.e. it is guaranteed to be safe to destroy them.
 */
unsigned
ebr_gc_epoch(ebr_t *ebr)
{
	/*
	 * Since we use only 3 epochs, e-2 is just the next global
	 * epoch with clock arithmetics.
	 */
	return (ebr->global_epoch + 1) % 3;
}
