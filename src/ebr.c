/*
 * Copyright (c) 2016 Mindaugas Rasiukevicius <rmind at noxt eu>
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
 * Summary
 *
 * The threads can only access global objects in an epoch which they
 * indicate before executing the critical path.  The objects in a target
 * epoch can be garbage collect after two successful increments of the
 * global epoch.  See the comments in the ebr_sync() function.
 *
 * Note: only three epochs are needed (e, e-1 and e-2), thus we use
 * clock arithmetics.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "ebr.h"
#include "utils.h"

typedef struct ebr_tls {
	/*
	 * - A local epoch counter for each thread.
	 * - An "active" flag for each thread.
	 * - Thread list entry (pointer).
	 */
	unsigned		active;
	unsigned		local_epoch;
	struct ebr_tls *	next;
} ebr_tls_t;

struct ebr {
	/*
	 * - There is a global epoch counter which can be 0, 1 or 2.
	 * - A global list of garbage collected objects for each epoch.
	 * - TLS with a list of the registered threads.
	 */
	unsigned		global_epoch;
	pthread_key_t		tls_key;
	ebr_tls_t *		list;
};

ebr_t *
ebr_create(void)
{
	ebr_t *ebr;

	if ((ebr = calloc(1, sizeof(ebr_t))) == NULL) {
		return NULL;
	}
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
 * ebr_register: register the current thread for EBR.
 *
 * => Returns 0 on success and errno on failure.
 */
int
ebr_register(ebr_t *ebr)
{
	ebr_tls_t *t, *head;

	t = pthread_getspecific(ebr->tls_key);
	if (__predict_false(t == NULL)) {
		if ((t = malloc(sizeof(ebr_tls_t))) == NULL) {
			return ENOMEM;
		}
		pthread_setspecific(ebr->tls_key, t);
	}
	memset(t, 0, sizeof(ebr_tls_t));

	do {
		head = ebr->list;
		t->next = head;
	} while (!atomic_compare_exchange_weak(&ebr->list, head, t));

	return 0;
}

/*
 * ebr_enter: mark the entrance to the critical path.
 */
void
ebr_enter(ebr_t *ebr)
{
	ebr_tls_t *t;

	t = pthread_getspecific(ebr->tls_key);
	ASSERT(t != NULL);

	/*
	 * Set the "active" flag and set the local epoch to global
	 * epoch (i.e. observe the global epoch).  Ensure that the
	 * epoch is observed before any loads in the critical path.
	 */
	t->active = true;
	t->local_epoch = ebr->global_epoch;
	atomic_thread_fence(memory_order_acquire);
}

/*
 * ebr_exit: mark the exist of the critical path.
 */
void
ebr_exit(ebr_t *ebr)
{
	ebr_tls_t *t;

	t = pthread_getspecific(ebr->tls_key);
	ASSERT(t != NULL);

	/*
	 * Clear the "active" flag.  Must ensure that any stores in the
	 * critical path reach global visibility before that.
	 */
	atomic_thread_fence(memory_order_release);
	t->active = false;
}

/*
 * ebr_sync: attempt to synchronise and announce a new epoch.
 *
 * => Return true on success and false if not ready.
 * => Synchronisation points must be serialised.
 */
bool
ebr_sync(ebr_t *ebr, unsigned *gc_epoch)
{
	unsigned epoch;
	ebr_tls_t *t;

	/*
	 * Check whether all active threads observed the global epoch.
	 */
	epoch = ebr->global_epoch;
	t = ebr->list;
	while (t) {
		if (t->active && t->local_epoch != epoch) {
			/* No, not ready. */
			*gc_epoch = ebr_gc_epoch(ebr);
			return false;
		}
		t = t->next;
	}

	/* Yes: increment and announce a new global epoch. */
	ebr->global_epoch = (epoch + 1) % 3;

	/*
	 * Let the new global epoch be 'e'.  At this point:
	 *
	 * => Active threads: may still be executing the critical path in
	 *    the e-1 epoch or may be entering a new critical path and
	 *    observing the new epoch e.
	 *
	 * => Inactive threads: may become active by entering a critical
	 *    path and observing the new epoch e.
	 *
	 * => There can be no threads still running the critical path in
	 *    the e-2 epoch.  This is the epoch ready for G/C.
	 */
	*gc_epoch = ebr_gc_epoch(ebr);
	return true;
}

unsigned
ebr_gc_epoch(ebr_t *ebr)
{
	/*
	 * Since we use only 3 epochs, e-2 is just the next global
	 * epoch with clock arithmetics.
	 */
	return (ebr->global_epoch + 1) % 3;
}
