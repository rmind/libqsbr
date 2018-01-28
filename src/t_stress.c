/*
 * Copyright (c) 2016-2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>

#include "ebr.h"
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
	char			_pad[
	    CACHE_LINE_SIZE - (sizeof(unsigned int *) + sizeof(int) * 2)];
} data_struct_t;

#define	DS_COUNT		4
#define	MAGIC_VAL		0x5a5a5a5a
#define	EPOCH_OFF		EBR_EPOCHS

static unsigned			magic_val = MAGIC_VAL;
static ebr_t *			ebr;
static data_struct_t		ds[DS_COUNT]
    __attribute__((__aligned__(CACHE_LINE_SIZE)));

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
		assert(obj->visible);
		obj->visible = false;
		obj->gc_epoch = EBR_EPOCHS + ebr_pending_epoch(ebr);

	} else if (!obj->gc_epoch) {
		/*
		 * Data structure is not globally visible.  Set the value
		 * and make it visible (think of the "insert" semantics).
		 */
		obj->ptr = &magic_val;
		atomic_thread_fence(memory_order_release);

		assert(!obj->visible);
		obj->visible = true;
	} else {
		/* Invisible, but not yet reclaimed. */
		assert(obj->gc_epoch != 0);
	}

	ebr_sync(ebr, &epoch);

	for (unsigned i = 0; i < DS_COUNT; i++) {
		if (obj->gc_epoch == EPOCH_OFF + epoch) {
			obj->ptr = NULL;
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
		if (ds[n].visible && *ds[n].ptr != MAGIC_VAL) {
			abort();
		}
		ebr_exit(ebr);
	}
	pthread_barrier_wait(&barrier);
	pthread_exit(NULL);
	return NULL;
}

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
	nworkers = sysconf(_SC_NPROCESSORS_CONF) + 1;
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
	ebr_destroy(ebr);
}

int
main(int argc, char **argv)
{
	if (argc >= 2) {
		nsec = (unsigned)atoi(argv[1]);
	}
	puts("stress test");
	run_test(ebr_stress);
	puts("ok");
	return 0;
}
