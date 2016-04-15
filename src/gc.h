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

__BEGIN_DECLS

gc_t *	gc_create(void (*)(gc_entry_t *));
void	gc_destroy(gc_t *);
void	gc_register(gc_t *);
void	gc_checkpoint(gc_t *);

void	gc_limbo(gc_t *, gc_entry_t *);
void	gc_async_flush(gc_t *);
void	gc_full_flush(gc_t *, unsigned);

__END_DECLS

#endif
