/*
 * Copyright (c) 2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "gc.h"

typedef struct {
	bool		destroyed;
	gc_entry_t	entry;
} obj_t;

static void
free_objs(gc_t *gc, gc_entry_t *entry)
{
	while (entry) {
		obj_t *obj;

		obj = (void *)((uintptr_t)entry - offsetof(obj_t, entry));
		entry = entry->next;

		obj->destroyed = true;
	}
	(void)gc;
}

static void
test_basic(void)
{
	gc_t *gc;
	obj_t obj;

	gc = gc_create(free_objs, 0);
	assert(gc != NULL);

	/*
	 * Basic critical path.
	 */
	gc_register(gc);
	gc_crit_enter(gc);
	gc_crit_exit(gc);

	/*
	 * Basic reclaim.
	 */
	memset(&obj, 0, sizeof(obj));
	assert(!obj.destroyed);

	gc_limbo(gc, &obj.entry);
	gc_cycle(gc);
	assert(obj.destroyed);

	/*
	 * Basic reclaim.
	 */
	memset(&obj, 0, sizeof(obj));
	assert(!obj.destroyed);

	gc_limbo(gc, &obj.entry);
	gc_cycle(gc);
	assert(obj.destroyed);

	/*
	 * Active reference.
	 */
	memset(&obj, 0, sizeof(obj));
	assert(!obj.destroyed);

	gc_limbo(gc, &obj.entry);
	assert(!obj.destroyed);

	gc_crit_enter(gc);
	gc_cycle(gc);
	assert(!obj.destroyed);

	gc_crit_exit(gc);
	gc_cycle(gc);
	assert(obj.destroyed);

	/*
	 * Full call.
	 */
	memset(&obj, 0, sizeof(obj));
	assert(!obj.destroyed);

	gc_limbo(gc, &obj.entry);
	gc_full(gc, 1);
	assert(obj.destroyed);

	gc_destroy(gc);
}

int
main(void)
{
	test_basic();
	puts("ok");
	return 0;
}
