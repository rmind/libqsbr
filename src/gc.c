/*
 * Copyright (c) 2015-2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

/*
 * Garbage Collection (G/C) interface for multi-threaded environment,
 * using the Epoch-based reclamation (EBR) mechanism.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>

#include "gc.h"
#include "ebr.h"
#include "utils.h"

struct gc {
	/*
	 * Objects are first inserted into the limbo list.  They move
	 * to a current epoch list on a G/C cycle.
	 */
	gc_entry_t *	limbo;

	/*
	 * A separate list for each epoch.  Objects in each list
	 * are reclaimed incrementally, as ebr_sync() announces new
	 * epochs ready to be reclaimed.
	 */
	gc_entry_t *	epoch_list[EBR_EPOCHS];

	/*
	 * EBR object and the reclamation function.
	 */
	ebr_t *		ebr;
	unsigned	entry_off;
	gc_func_t	reclaim;
	void *		arg;
};

static void
gc_default_reclaim(gc_entry_t *entry, void *arg)
{
	gc_t *gc = arg;
	const unsigned off = gc->entry_off;
	void *obj;

	while (entry) {
		obj = (void *)((uintptr_t)entry - off);
		entry = entry->next;
		free(obj);
	}
}

gc_t *
gc_create(unsigned off, gc_func_t reclaim, void *arg)
{
	gc_t *gc;

	if ((gc = calloc(1, sizeof(gc_t))) == NULL) {
		return NULL;
	}
	gc->ebr = ebr_create();
	if (!gc->ebr) {
		free(gc);
		return NULL;
	}
	gc->entry_off = off;
	if (reclaim) {
		gc->reclaim = reclaim;
		gc->arg = arg;
	} else {
		gc->reclaim = gc_default_reclaim;
		gc->arg = gc;
	}
	return gc;
}

void
gc_destroy(gc_t *gc)
{
	for (unsigned i = 0; i < EBR_EPOCHS; i++) {
		ASSERT(gc->epoch_list[i] == NULL);
	}
	ASSERT(gc->limbo == NULL);

	ebr_destroy(gc->ebr);
	free(gc);
}

void
gc_register(gc_t *gc)
{
	ebr_register(gc->ebr);
}

void
gc_crit_enter(gc_t *gc)
{
	ebr_enter(gc->ebr);
}

void
gc_crit_exit(gc_t *gc)
{
	ebr_exit(gc->ebr);
}

/*
 * gc_limbo: insert into the limbo list.
 */
void
gc_limbo(gc_t *gc, void *obj)
{
	gc_entry_t *ent = (void *)((uintptr_t)obj + gc->entry_off);
	gc_entry_t *head;

	do {
		head = gc->limbo;
		ent->next = head;
	} while (!atomic_compare_exchange_weak(&gc->limbo, head, ent));
}

void
gc_cycle(gc_t *gc)
{
	unsigned count = EBR_EPOCHS, gc_epoch, staging_epoch;
	ebr_t *ebr = gc->ebr;
	gc_entry_t *gc_list;
next:
	/*
	 * Call the EBR synchronisation and check whether it announces
	 * a new epoch.
	 */
	if (!ebr_sync(ebr, &gc_epoch)) {
		/* Not announced -- not ready to reclaim. */
		return;
	}

	/*
	 * Move the objects from the limbo list into the staging epoch.
	 */
	staging_epoch = ebr_staging_epoch(ebr);
	ASSERT(gc->epoch_list[staging_epoch] == NULL);
	gc->epoch_list[staging_epoch] = atomic_exchange(&gc->limbo, NULL);

	/*
	 * Reclaim the objects in the G/C epoch list.
	 */
	gc_list = gc->epoch_list[gc_epoch];
	if (!gc_list && count--) {
		/*
		 * If there is nothing to G/C -- try a next epoch,
		 * but loop only for one "full" cycle.
		 */
		goto next;
	}
	gc->reclaim(gc_list, gc->arg);
	gc->epoch_list[gc_epoch] = NULL;
}

void
gc_full(gc_t *gc, unsigned msec_retry)
{
	const struct timespec dtime = { 0, msec_retry * 1000 * 1000 };
	unsigned count = SPINLOCK_BACKOFF_MIN;
	bool done;
again:
	/*
	 * Run a G/C cycle.
	 */
	gc_cycle(gc);

	/*
	 * Check all epochs and the limbo list.
	 */
	done = true;
	for (unsigned i = 0; i < EBR_EPOCHS; i++) {
		if (gc->epoch_list[i]) {
			done = false;
			break;
		}
	}
	if (!done || gc->limbo) {
		/*
		 * There are objects waiting for reclaim.  Spin-wait or
		 * sleep for a little bit and try to reclaim them.
		 */
		if (count < SPINLOCK_BACKOFF_MAX) {
			SPINLOCK_BACKOFF(count);
		} else {
			(void)nanosleep(&dtime, NULL);
		}
		goto again;
	}
}
