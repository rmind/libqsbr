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
