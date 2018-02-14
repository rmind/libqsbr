# Quiescent-State and Epoch based reclamation

[![Build Status](https://travis-ci.org/rmind/libqsbr.svg?branch=master)](https://travis-ci.org/rmind/libqsbr)

Epoch-Based Reclamation (EBR) and Quiescent-State-Based Reclamation (QSBR)
are synchronisation mechanisms which can be used for efficient memory/object
reclamation (garbage collection) in concurrent environment.  Conceptually
they are very similar to the read-copy-update (RCU) mechanism.

EBR and QSBR are simpler, more lightweight and often faster than RCU.
However, each thread must register itself when using these mechanisms.
EBR allows user to mark the critical code paths without the need to
periodically indicate the quiescent state.  It is slightly slower than
QSBR due to the need to issue a memory barrier on the reader side.
QSBR is more lightweight, but each thread must manually indicate the
quiescent state i.e. threads must periodically pass a checkpoint where
they call a dedicated function.  In many applications, such approach
can be practical.

A typical use case of the EBR or QSBR would be together with lock-free
data structures.  This library provides raw EBR and QSBR mechanisms as
well as a higher level a garbage collection (GC) interface based on EBR.

The implementation is written in C11 and distributed under the
2-clause BSD license.

## EBR API

* `ebr_t *ebr_create(void)`
  * Construct a new EBR object.

* `void ebr_destroy(ebr_t *ebr)`
  * Destroy the EBR object.

* `int ebr_register(ebr_t *ebr)`
  * Register the current thread for EBR synchronisation (typically,
  as a reader).  Returns zero on success and -1 on failure.

* `void ebr_enter(ebr_t *ebr)`
  * Mark the entrance to the critical path.  Typically, this would be
  used by the readers when accessing some shared data; reclamation of
  the objects is guaranteed to not occur in the critical path.

* `void ebr_exit(ebr_t *ebr)`
  * Mark the exist of the critical path.  Typically, after this point,
  reclamation may occur on some form of reference on shared data is
  acquired by the reader.

* `bool ebr_sync(ebr_t *ebr, unsigned *gc_epoch)`
  * Attempt to synchronise and announce a new epoch.  If a new epoch
  is announced, returns `true`.  In any case, the _epoch_ available for
  reclamation is returned.  The number of epochs is defined by the
  `EBR_EPOCHS` constant and the epoch value is `0 <= epoch < EBR_EPOCHS`.
  Note: the synchronisation points must be serialised (e.g. if there
  are multiple G/C workers or other writers); typically, calls to the
  `ebr_pending_epoch` and `ebr_gc_epoch` would be a part of the same
  serialised path.

* `unsigned ebr_pending_epoch(ebr_t *ebr)`
  * Returns an _epoch_ pending for reclamation.  This can be used as
  reference value for the pending queue/tag, used to postpone the
  reclamation until this epoch becomes available for G/C.  Note that
  this functions would typically be serialised together with the
  `ebr_sync` calls.

* `unsigned ebr_gc_epoch(ebr_t *ebr)`
  * Returns the _epoch_ available for reclamation.  The _epoch_ value
  shall be the same as returned by the last successful `ebr_sync` call.
  Note that these two functions would typically require the same form
  of serialisation.

## G/C API

* `gc_t *gc_create(gc_func_t reclaim, unsigned obj_off)`
  * Construct a new G/C management object.  A custom `reclaim` function
  can be passed for object destruction.  This function must process a list
  of objects, embedding `gc_entry_t` structure which is used for a form a
  list.  If _reclaim_ is NULL, then a default reclaim functions is used
  which call `free(3)` for each object.  Additionally, when the default
  reclaim function is used, the `obj_off` value will be used to decrement
  the pointer value before freeing the object; typically, this would be
  `offsetof(struct obj, gc_entry)` i.e. offset of the embedded G/C entry
  structure in the user object.

* `void gc_destroy(gc_t *gc)`
  * Destroy the G/C management object.

* `void gc_register(gc_t *gc)`
  * Register the current thread for G/C management.

* `void gc_crit_enter(gc_t *gc)`
  * Enter the critical path where objects would be referenced.  This
  prevents the G/C mechanism from reclaiming (destroying) the object.

* `void gc_crit_exit(gc_t *gc)`
  * Exit the critical path, indicating that the target objects no
  longer have active references and the G/C mechanism may consider
  them for reclamation.

* `void gc_limbo(gc_t *gc, gc_entry_t *entry)`
  * Insert the object (referenced via its G/C entry structure) into a
  "limbo" list, staging it for reclamation (destruction).  This is a
  request to reclaim the object once it is guaranteed that there are
  no threads referencing it in the critical path.

* `void gc_cycle(gc_t *gc)`
  * Run a G/C cycle, attempting to reclaim some objects which were
  added to the limbo list.  The objects which are no longer referenced
  may are not guaranteed to be reclaimed immediately after one cycle.
  This function does not block and is expected to be called periodically
  for an incremental object reclamation.

* `void gc_full(gc_t *gc, unsigned msec_retry)`
  * Run a full G/C in order to ensure that all staged objects have been
  reclaimed.  This function will block for `msec_retry` milliseconds before
  trying again, if there are objects which cannot be reclaimed immediately.

## Notes

The implementation was extensively tested on a 24-core x86 machine,
see [the stress test](src/t_stress.c) for the details on the technique.

## Examples ###

The G/C list for the objects to reclaim should be created by some master
thread.  It takes a reclaim function.
```c
static gc_t *	gc;

void
some_sysinit(void)
{
	gc = gc_create(NULL, offsetof(obj_t, gc_entry));
	assert(gc != NULL);
	...
}
```

An example code fragment of a reader thread:
```c
	gc_register(gc);

	while (!exit) {
		/*
		 * Some processing referencing the objects.  The readers
		 * must indicate the critical path where they actively
		 * reference any objects.
		 */
		gc_crit_enter(gc);
		obj = some_lookup();
		some_access(obj);
		gc_crit_exit(gc);
	}
}
```

Here is an example code fragment in the worker thread which illustrates
how the object would be staged for destruction (reclamation):
```c
	/*
	 * Remove the object from the lock-free container.  The
	 * object is no longer globally visible.  Add the object
	 * to the G/C limbo list (to be flushed later).
	 */
	obj = lockfree_remove(container, key);
	gc_limbo(gc, obj->gc_entry);

	...

	/*
	 * Checkpoint: run a G/C cycle, attempting to reclaim *some*
	 * objects previously added to the limbo list.  This should be
	 * called periodically, for incremental object reclamation.
	 *
	 * WARNING: Any gc_cycle() calls must be serialised (using a
	 * mutex or by running in a single-threaded manner).
	 */
	gc_cycle(gc);

	...

	/*
	 * Eventually, a full G/C might have to be performed to ensure
	 * that all objects have been reclaimed.  This call can block.
	 */
	gc_full(gc, 1); // sleep for 1 msec before re-checking
```
