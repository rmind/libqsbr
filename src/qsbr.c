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

#include <sys/queue.h>
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
	LIST_ENTRY(qsbr_tls)	entry;
} qsbr_tls_t;

struct qsbr {
	/*
	 * The global epoch, TLS key with a list of the registered threads.
	 */
	qsbr_epoch_t		global_epoch;
	pthread_key_t		tls_key;
	pthread_mutex_t		lock;
	LIST_HEAD(, qsbr_tls)	list;
};

qsbr_t *
qsbr_create(void)
{
	qsbr_t *qs;
	int ret;

	ret = posix_memalign((void **)&qs, CACHE_LINE_SIZE, sizeof(qsbr_t));
	if (ret != 0) {
		errno = ret;
		return NULL;
	}
	memset(qs, 0, sizeof(qsbr_t));

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
	qsbr_tls_t *t;

	t = pthread_getspecific(qs->tls_key);
	if (__predict_false(t == NULL)) {
		int ret;

		ret = posix_memalign((void **)&t,
		    CACHE_LINE_SIZE, sizeof(qsbr_tls_t));
		if (ret != 0) {
			errno = ret;
			return -1;
		}
		pthread_setspecific(qs->tls_key, t);
	}
	memset(t, 0, sizeof(qsbr_tls_t));

	pthread_mutex_lock(&qs->lock);
	LIST_INSERT_HEAD(&qs->list, t, entry);
	pthread_mutex_unlock(&qs->lock);
	return 0;
}

void
qsbr_unregister(qsbr_t *qsbr)
{
	qsbr_tls_t *t;

	t = pthread_getspecific(qsbr->tls_key);
	ASSERT(t != NULL);
	pthread_setspecific(qsbr->tls_key, NULL);

	pthread_mutex_lock(&qsbr->lock);
	LIST_REMOVE(t, entry);
	pthread_mutex_unlock(&qsbr->lock);
	free(t);
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

	/*
	 * Observe the current epoch and issue a load barrier.
	 *
	 * Additionally, issue a store barrier before observation,
	 * so the callers could assume qsbr_checkpoint() being a
	 * full barrier.
	 */
	atomic_thread_fence(memory_order_seq_cst);
	t->local_epoch = qs->global_epoch;
}

qsbr_epoch_t
qsbr_barrier(qsbr_t *qs)
{
	/* Note: atomic operation will issue a store barrier. */
	return atomic_fetch_add(&qs->global_epoch, 1) + 1;
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
	LIST_FOREACH(t, &qs->list, entry) {
		if (t->local_epoch < target) {
			/* Not ready to G/C. */
			return false;
		}
	}

	/* Detected the grace period. */
	return true;
}
