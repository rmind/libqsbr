/*
 * Copyright (c) 2015 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

/*
 * Garbage collection (GC) interface using the quiescent state based
 * reclamation (QSBR) mechanism.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "gc.h"
#include "qsbr.h"
#include "utils.h"

struct gc {
	/*
	 * Objects are first inserted into the limbo list.  They move
	 * to a stage list once the QSBR barrier is issued.
	 */
	gc_entry_t *	limbo;

	/*
	 * Stage list with a target epoch.  Once we observe the target
	 * epoch, the objects in this list can be reclaimed.
	 */
	qsbr_epoch_t	stage_epoch;
	gc_entry_t *	stage_list;
	bool		move_toggle;

	/* QSBR object and the reclamation function. */
	qsbr_t *	qsbr;
	void		(*reclaim)(gc_entry_t *);
};

gc_t *
gc_create(void (*reclaim)(gc_entry_t *))
{
	gc_t *gc;

	if ((gc = calloc(1, sizeof(gc_t))) == NULL) {
		return NULL;
	}
	gc->qsbr = qsbr_create();
	if (!gc->qsbr) {
		free(gc);
		return NULL;
	}
	gc->reclaim = reclaim;
	return gc;
}

void
gc_destroy(gc_t *gc)
{
	ASSERT(gc->limbo == NULL);
	ASSERT(gc->stage_list == NULL);

	qsbr_destroy(gc->qsbr);
	free(gc);
}

void
gc_register(gc_t *gc)
{
	qsbr_register(gc->qsbr);
}

void
gc_checkpoint(gc_t *gc)
{
	qsbr_checkpoint(gc->qsbr);
}

void
gc_limbo(gc_t *gc, gc_entry_t *ent)
{
	gc_entry_t *head;

	/*
	 * Lockless insert into the limbo list.
	 */
	do {
		head = gc->limbo;
		ent->next = head;
	} while (!atomic_compare_exchange_weak(&gc->limbo, head, ent));
}

void
gc_async_flush(gc_t *gc)
{
	qsbr_t *qsbr = gc->qsbr;

	/*
	 * First, check whether the previous (stage target) epoch has
	 * been globally observed.  If so, we can reclaim the stage list.
	 */
	if (gc->stage_list && qsbr_sync(qsbr, gc->stage_epoch)) {
		gc->reclaim(gc->stage_list);
		gc->stage_list = NULL;
	}

	/*
	 * If the limbo list does not have new entries - nothing to do.
	 */
	if (gc->limbo == NULL) {
		return;
	}

	/*
	 * Move the entries from limbo to stage list, if it is empty.
	 */
	if (gc->stage_list == NULL) {
		gc->stage_list = atomic_exchange(&gc->limbo, NULL);
	}

	/*
	 * Issue a QSBR barrier which returns a new epoch every second
	 * requests.  Otherwise, we might have a constantly moving target
	 * epoch when stage list reclamation is never ready.
	 */
	gc->move_toggle = !gc->move_toggle;
	if (gc->move_toggle) {
		/* New stage target! */
		gc->stage_epoch = qsbr_barrier(qsbr);
	}
}

void
gc_full_flush(gc_t *gc, unsigned msec_delta)
{
	const struct timespec dtime = { 0, msec_delta * 1000 * 1000 };

	while (gc_async_flush(gc), (gc->limbo || gc->stage_list)) {
		(void)nanosleep(&dtime, NULL);
	}
}
