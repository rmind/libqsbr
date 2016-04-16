/*
 * Copyright (c) 2015 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#ifndef	_QSBR_H_
#define	_QSBR_H_

#include <sys/cdefs.h>
#include <stdbool.h>

struct qsbr;
typedef struct qsbr qsbr_t;
typedef unsigned long qsbr_epoch_t;

__BEGIN_DECLS

qsbr_t *	qsbr_create(void);
void		qsbr_destroy(qsbr_t *);

int		qsbr_register(qsbr_t *);
void		qsbr_checkpoint(qsbr_t *);
qsbr_epoch_t	qsbr_barrier(qsbr_t *);
bool		qsbr_sync(qsbr_t *, qsbr_epoch_t);

__END_DECLS

#endif
