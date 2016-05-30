# Quiescent-State and Epoch based reclamation

Quiescent-State-Based Reclamation (QSBR) and Epoch-Based Reclamation (EBR)
are synchronisation mechanisms which can be used for efficient memory
reclamation (garbage collection) in multi-threaded environment.
Conceptually they are very similar to the read-copy-update (RCU) mechanism.

QSBR is simpler, more lightweight and faster than RCU.  However, unlike RCU,
it requires *each* thread to register and manually indicate the quiescent
state i.e. threads must periodically pass a checkpoint.  In practice, many
applications can easily do that.  As a more convenient alternative, EBR
allows user to mark the critical code paths without the need to periodically
indicate the quiescent state.  It is slightly slower than QSBR due to the
need to issue a memory barrier on the reader side.

A typical use case of the QSBR or EBR would be together with lock-free data
structures.  This library provides a raw QSBR and EBR mechanisms as well as
a higher level a garbage collection (GC) interface based on QSBR.

The libqsbr library is released under the BSD license.

## EBR API

* `ebr_t *ebr_create(void)`
  * Construct a new EBR object.

* `void ebr_destroy(ebr_t *ebr)`
  * Destroy the EBR object.

* `int ebr_register(ebr_t *ebr)`
  * Register the current thread for EBR synchronisation (as a reader).
  Returns zero on success and -1 on failure.

* `void ebr_enter(ebr_t *ebr)`
  * Mark the entrance to the critical path.  Typically, this would be
  used by the readers when accessing some shared data; reclamation of
  the objects is guaranteed to not occur in the critical path.

* `void ebr_exit(ebr_t *ebr)`
  * Mark the exist of the critical path.  Typically, after this point,
  reclamation may occur on some form of reference on shared data is
  acquired by the reader.

* `bool ebr_sync(ebr_t *ebr, unsigned *gc_epoch)`
  * Attempt to synchronise and announce a new epoch.  On success, returns
  `true` and the _epoch_ available for reclamation; returns false if not
  ready.  The number of epochs is defined by the `EBR_EPOCHS` constant and
  the epoch value is `0 <= epoch < EBR_EPOCHS`.  Note: the synchronisations
  points must be serialised (e.g. if there are multiple G/C thread or other
  forms of writers).

* `unsigned ebr_gc_epoch(ebr_t *ebr)`
  * Returns the current _epoch_ available for reclamation.  The _epoch_
  value shall be the same as returned by the last successful `ebr_sync`
  call.  Note that these two functions would typically require the same
  form of serialisation.

## Examples ###

The G/C list for the objects to reclaim should be created by some master
thread.  It takes a reclaim function.
```c
static gc_t *	gc;

static void
obj_reclaim(gc_entry_t *entry)
{
	/*
	 * Note: a list of entries is passed to the reclaim function.
	 */
	while (entry) {
		obj_t *obj;

		/* Destroy the actual object; at this point it is safe. */
		obj = (obj_t *)((uintptr_t)entry - offsetof(obj_t, gc_entry));
		entry = entry->next;
		free(obj);
	}
}

void
some_sysinit(void)
{
	gc = gc_create(obj_reclaim);
	assert(gc != NULL);
	...
}
```

Each thread which can reference an object must register:
```c
static void *
worker_thread(void *arg)
{
	gc_register(gc);

	while (!exit) {
		/*
		 * Some processing referencing the objects..
		 */
		...

		/*
		 * Checkpoint: indicate that the thread is in the
		 * quiescient state -- at this point, we no longer
		 * actively reference any objects.  This is cheap
		 * and can be invoked more frequently.
		 */
		gc_checkpoint(gc);

		/*
		 * Perform an asynchronous flush: attempt to reclaim the
		 * objects previously added to the list; if they are not
		 * ready to be reclaimed - the function just returns, so
		 * the flush should be invoked periodically.
		 */
		gc_async_flush(gc);
	}
	pthread_exit(NULL);
}
```

Here is an example code fragment in the worker thread which illustrates
how the object would be staged for destruction (reclamation):
```c
	foreach key in key_list {
		/*
		 * Remove the object from the lock-free container.  The
		 * object is no longer globally visible.  Add the object
		 * to the G/C limbo list (to be flushed later).
		 */
		obj = lockfree_remove(container, key);
		gc_limbo(gc, obj->gc_entry);
	}
```
