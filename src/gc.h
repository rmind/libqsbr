/*
 * Copyright (c) 2015 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#ifndef _GC_H_
#define _GC_H_

#include <sys/cdefs.h>

typedef struct gc gc_t;

typedef struct gc_entry {
	struct gc_entry *next;
} gc_entry_t;

typedef void (*gc_func_t)(gc_entry_t *, void *);

__BEGIN_DECLS

gc_t *	gc_create(unsigned, gc_func_t, void *);
void	gc_destroy(gc_t *);
void	gc_register(gc_t *);
void	gc_unregister(gc_t *);

void	gc_crit_enter(gc_t *);
void	gc_crit_exit(gc_t *);

void	gc_limbo(gc_t *, void *);
void	gc_cycle(gc_t *);
void	gc_full(gc_t *, unsigned);

__END_DECLS

#endif
