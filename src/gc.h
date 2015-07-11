/*
 * Copyright (c) 2015 Mindaugas Rasiukevicius <rmind at netbsd org>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#ifndef _GC_H_
#define _GC_H_

typedef struct gc gc_t;

typedef struct gc_entry {
	struct gc_entry *next;
} gc_entry_t;

gc_t *	gc_create(void (*)(gc_entry_t *));
void	gc_destroy(gc_t *);
void	gc_register(gc_t *);
void	gc_checkpoint(gc_t *);

void	gc_limbo(gc_t *, gc_entry_t *);
void	gc_async_flush(gc_t *);
void	gc_full_flush(gc_t *, unsigned);

#endif
