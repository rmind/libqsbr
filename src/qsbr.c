/*
 * Copyright (c) 2015 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

/*
 * Quiescent state based reclamation (QSBR).
 *
 * Notes on the usage:
 *
 * Each registered thread has to periodically indicate that it is in a
 * quiescent i.e. the state when it does not hold any memory references
 * to the objects which may be garbage collected.  A typical use of the
 * qsbr_checkpoint() function would be e.g. after processing a single
 * request when any shared state is no longer referenced.  The higher
 * the period, the higher the reclamation granularity.
 *
 * Writers i.e. threads which are trying to garbage collect the object
 * should ensure that the objects are no longer globally visible and
 * then issue a barrier using qsbr_barrier() function.  This function
 * returns a generation number.  It is safe to reclaim the said objects
 * when qsbr_sync() returns true on a given number.
 *
 * Note that this interface is asynchronous.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "qsbr.h"
#include "utils.h"

/*
 * FIXME: handle the epoch overflow on 32-bit systems; not a problem
 * on 64-bit systems.
 */
static_assert(sizeof(qsbr_epoch_t) == 8, "expected 64-bit counter");

typedef struct qsbr_tls {
	/*
	 * The thread (local) epoch, observed at qsbr_checkpoint().
	 * Also, a pointer to the TLS structure of a next thread.
	 */
	qsbr_epoch_t		local_epoch;
	struct qsbr_tls *	next;
} qsbr_tls_t;

struct qsbr {
	/*
	 * The global epoch, TLS key with a list of the registered threads.
	 */
	qsbr_epoch_t		global_epoch;
	pthread_key_t		tls_key;
	qsbr_tls_t *		list;
};

qsbr_t *
qsbr_create(void)
{
	qsbr_t *qs;

	if ((qs = calloc(1, sizeof(qsbr_t))) == NULL) {
		return NULL;
	}
	if (pthread_key_create(&qs->tls_key, free) != 0) {
		free(qs);
		return NULL;
	}
	qs->global_epoch = 1;
	return qs;
}

void
qsbr_destroy(qsbr_t *qs)
{
	pthread_key_delete(qs->tls_key);
	free(qs);
}

/*
 * qsbr_register: register the current thread for QSBR.
 */
int
qsbr_register(qsbr_t *qs)
{
	qsbr_tls_t *t, *head;

	t = pthread_getspecific(qs->tls_key);
	if (__predict_false(t == NULL)) {
		if ((t = malloc(sizeof(qsbr_tls_t))) == NULL) {
			return ENOMEM;
		}
		pthread_setspecific(qs->tls_key, t);
	}
	memset(t, 0, sizeof(qsbr_tls_t));

	do {
		head = qs->list;
		t->next = head;
	} while (!atomic_compare_exchange_weak(&qs->list, head, t));

	return 0;
}

/*
 * qsbr_checkpoint: indicate a quiescent state of the current thread.
 */
void
qsbr_checkpoint(qsbr_t *qs)
{
	qsbr_tls_t *t;

	t = pthread_getspecific(qs->tls_key);
	ASSERT(t != NULL);

	/* Observe the current epoch. */
	atomic_thread_fence(memory_order_acquire);
	t->local_epoch = qs->global_epoch;
	atomic_thread_fence(memory_order_release);
}

qsbr_epoch_t
qsbr_barrier(qsbr_t *qs)
{
	/* Note: atomic operation will issue a store barrier. */
	return atomic_fetch_add(&qs->global_epoch, 1);
}

bool
qsbr_sync(qsbr_t *qs, qsbr_epoch_t target)
{
	qsbr_tls_t *t;

	/*
	 * First, our thread should observe the epoch itself.
	 */
	qsbr_checkpoint(qs);

	/*
	 * Have all threads observed the target epoch?
	 */
	t = qs->list;
	while (t) {
		if (t->local_epoch < target) {
			/* Not ready to G/C. */
			return false;
		}
		t = t->next;
	}

	/* Detected the grace period. */
	return true;
}
