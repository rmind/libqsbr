/*
 * Copyright (c) 2016-2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>

#include "ebr.h"
#include "qsbr.h"
#include "gc.h"
#include "utils.h"

static unsigned			nsec = 10; /* seconds */

static pthread_barrier_t	barrier;
static unsigned			nworkers;
static volatile bool		stop;

#define	CACHE_LINE_SIZE		64

typedef struct {
	unsigned int *		ptr;
	unsigned int		visible;
	unsigned int		gc_epoch;
	gc_entry_t		gc_entry;
	char			_pad[CACHE_LINE_SIZE - 8 - 4 - 4 - 8];
} data_struct_t;

#define	DS_COUNT		4
#define	MAGIC_VAL		0x5a5a5a5a
#define	EPOCH_OFF		EBR_EPOCHS

static unsigned			magic_val = MAGIC_VAL;

static ebr_t *			ebr;
static qsbr_t *			qsbr;
static gc_t *			gc;

static data_struct_t		ds[DS_COUNT]
    __attribute__((__aligned__(CACHE_LINE_SIZE)));
static uint64_t			destructions;

static void
access_obj(data_struct_t *obj)
{
	atomic_thread_fence(memory_order_acquire);
	if (obj->visible && *obj->ptr != MAGIC_VAL) {
		abort();
	}
}

static void
mock_insert_obj(data_struct_t *obj)
{
	obj->ptr = &magic_val;
	atomic_thread_fence(memory_order_release);
	assert(!obj->visible);
	obj->visible = true;
}

static void
mock_remove_obj(data_struct_t *obj)
{
	assert(obj->visible);
	obj->visible = false;
}

static void
mock_destroy_obj(data_struct_t *obj)
{
	obj->ptr = NULL;
	destructions++;
}

/*
 * EBR stress test.
 */

static void
ebr_writer(unsigned target)
{
	data_struct_t *obj = &ds[target];
	unsigned epoch;

	if (obj->visible) {
		/*
		 * The data structure is visible.  First, ensure it is no
		 * longer visible (think of "remove" semantics).
		 */
		mock_remove_obj(obj);
		obj->gc_epoch = EBR_EPOCHS + ebr_staging_epoch(ebr);

	} else if (!obj->gc_epoch) {
		/*
		 * Data structure is not globally visible.  Set the value
		 * and make it visible (think of the "insert" semantics).
		 */
		mock_insert_obj(obj);
	} else {
		/* Invisible, but not yet reclaimed. */
		assert(obj->gc_epoch != 0);
	}

	ebr_sync(ebr, &epoch);

	for (unsigned i = 0; i < DS_COUNT; i++) {
		if (obj->gc_epoch == EPOCH_OFF + epoch) {
			mock_destroy_obj(obj);
			obj->gc_epoch = 0;
		}
	}
}

static void *
ebr_stress(void *arg)
{
	const unsigned id = (uintptr_t)arg;
	unsigned n = 0;

	ebr_register(ebr);

	/*
	 * There are NCPU threads concurrently reading data and a single
	 * writer thread (ID 0) modifying data.  The writer will modify
	 * the pointer used by the readers to NULL as soon as it considers
	 * the object ready for reclaim.
	 */

	pthread_barrier_wait(&barrier);
	while (!stop) {
		n = ++n & (DS_COUNT - 1);

		if (id == 0) {
			ebr_writer(n);
			continue;
		}

		/*
		 * Reader: iterate through the data structures and,
		 * if the object is visible (think of "lookup" semantics),
		 * read its value through a pointer.  The writer will set
		 * the pointer to NULL when it thinks the object is ready
		 * to be reclaimed.
		 *
		 * Incorrect reclamation mechanism would lead to the crash
		 * in the following pointer dereference.
		 */
		ebr_enter(ebr);
		access_obj(&ds[n]);
		ebr_exit(ebr);
	}
	pthread_barrier_wait(&barrier);
	pthread_exit(NULL);
	return NULL;
}

/*
 * QSBR stress test.
 */

static void
qsbr_writer(unsigned target)
{
	data_struct_t *obj = &ds[target];

	/*
	 * See the ebr_writer() function for more details.
	 */
	if (obj->visible) {
		unsigned count = SPINLOCK_BACKOFF_MIN;
		qsbr_epoch_t target_epoch;

		mock_remove_obj(obj);

		/* QSBR synchronisation barrier. */
		target_epoch = qsbr_barrier(qsbr);
		while (!qsbr_sync(qsbr, target_epoch)) {
			SPINLOCK_BACKOFF(count);
			if (stop) {
				/*
				 * Other threads might have exited and
				 * the checkpoint would never be passed.
				 */
				return;
			}
		}

		/* It is safe to "destroy" the object now. */
		mock_destroy_obj(obj);
	} else {
		mock_insert_obj(obj);
	}
}

static void *
qsbr_stress(void *arg)
{
	const unsigned id = (uintptr_t)arg;
	unsigned n = 0;

	/*
	 * See the ebr_stress() function for explanation.
	 */

	qsbr_register(qsbr);
	pthread_barrier_wait(&barrier);
	while (!stop) {
		n = ++n & (DS_COUNT - 1);
		if (id == 0) {
			qsbr_writer(n);
			continue;
		}
		access_obj(&ds[n]);
		qsbr_checkpoint(qsbr);
	}
	pthread_barrier_wait(&barrier);
	pthread_exit(NULL);
	return NULL;
}

/*
 * G/C stress test.
 */

static void
gc_func(gc_entry_t *entry, void *arg)
{
	const unsigned off = offsetof(data_struct_t, gc_entry);

	while (entry) {
		data_struct_t *obj;

		obj = (void *)((uintptr_t)entry - off);
		entry = entry->next;
		mock_destroy_obj(obj);
	}
}

static void
gc_writer(unsigned target)
{
	data_struct_t *obj = &ds[target];

	if (obj->visible) {
		mock_remove_obj(obj);
		gc_limbo(gc, obj);
	} else if (!obj->ptr) {
		mock_insert_obj(obj);
	}
	gc_cycle(gc);
}

static void *
gc_stress(void *arg)
{
	const unsigned id = (uintptr_t)arg;
	unsigned n = 0;

	/*
	 * See the ebr_stress() function for explanation.
	 */

	gc_register(gc);
	pthread_barrier_wait(&barrier);
	while (!stop) {
		n = ++n & (DS_COUNT - 1);
		if (id == 0) {
			gc_writer(n);
			continue;
		}
		gc_crit_enter(gc);
		access_obj(&ds[n]);
		gc_crit_exit(gc);
	}
	pthread_barrier_wait(&barrier);
	pthread_exit(NULL);
	return NULL;
}

/*
 * Helper routines
 */

static void
ding(int sig)
{
	(void)sig;
	stop = true;
}

static void
run_test(void *func(void *))
{
	struct sigaction sigalarm;
	pthread_t *thr;
	int ret;

	/*
	 * Setup the threads.
	 */
	nworkers = sysconf(_SC_NPROCESSORS_CONF);
	thr = calloc(nworkers, sizeof(pthread_t));
	pthread_barrier_init(&barrier, NULL, nworkers);
	stop = false;

	memset(&sigalarm, 0, sizeof(struct sigaction));
	sigalarm.sa_handler = ding;
	ret = sigaction(SIGALRM, &sigalarm, NULL);
	assert(ret == 0); (void)ret;

	/*
	 * Create some data structures and the EBR object.
	 */
	memset(&ds, 0, sizeof(ds));
	ebr = ebr_create();
	qsbr = qsbr_create();
	gc = gc_create(offsetof(data_struct_t, gc_entry), gc_func, NULL);
	destructions = 0;

	/*
	 * Spin the test.
	 */
	alarm(nsec);

	for (unsigned i = 0; i < nworkers; i++) {
		if ((errno = pthread_create(&thr[i], NULL,
		    func, (void *)(uintptr_t)i)) != 0) {
			err(EXIT_FAILURE, "pthread_create");
		}
	}
	for (unsigned i = 0; i < nworkers; i++) {
		pthread_join(thr[i], NULL);
	}
	pthread_barrier_destroy(&barrier);
	printf("# %"PRIu64"\n", destructions);

	ebr_destroy(ebr);
	qsbr_destroy(qsbr);

	gc_full(gc, 1);
	gc_destroy(gc);
}

int
main(int argc, char **argv)
{
	if (argc >= 2) {
		nsec = (unsigned)atoi(argv[1]);
	}
	puts("stress test");
	run_test(ebr_stress);
	run_test(qsbr_stress);
	run_test(gc_stress);
	puts("ok");
	return 0;
}
