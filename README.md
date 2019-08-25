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
well as a higher level garbage collection (GC) interface based on EBR.

The implementation is written in C11 and distributed under the
2-clause BSD license.

References:

	K. Fraser, Practical lock-freedom,
	Technical Report UCAM-CL-TR-579, February 2004
	https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-579.pdf

	T. E. Hart, P. E. McKenney, A.D. Brown,
	Making Lockless Synchronization Fast: Performance Implications of Memory Reclamation.
	Parallel and Distributed Processing Symposium, April 2006.
	http://csng.cs.toronto.edu/publication_files/0000/0165/ipdps06.pdf

## EBR API

* `ebr_t *ebr_create(void)`
  * Construct a new EBR object.

* `void ebr_destroy(ebr_t *ebr)`
  * Destroy the EBR object.

* `int ebr_register(ebr_t *ebr)`
  * Register the current thread for EBR synchronisation.  Returns 0 on
  success and -1 on failure.  Note: each reader thread (i.e. callers of
  `ebr_enter/ebr_exit`) **must** register.

* `void ebr_unregister(ebr_t *ebr)`
  * Remove the current thread from the EBR synchronisation list.  Each
  registered thread must leave the list before the exit (this may be not
  necessary if all threads exit together).  It is the caller's responsibility
  to synchronise the thread exit, if needed.

* `void ebr_enter(ebr_t *ebr)`
  * Mark the entrance to the critical path.  Typically, this would be
  used by the readers when accessing some shared data; reclamation of
  objects is guaranteed to not occur in the critical path.
  * Note: the EBR mechanism is not limited to the concept of "objects".
  It can be any form of reference to the globally shared data.

* `void ebr_exit(ebr_t *ebr)`
  * Mark the exit of the critical path.  Reclamation of the shared data
  may occur after this point.

* `bool ebr_sync(ebr_t *ebr, unsigned *gc_epoch)`
  * Attempt to synchronise and announce a new epoch.  Returns `true` if
  a new epoch is announced and `false` otherwise.  In either case, the
  _epoch_ available for reclamation is returned.  The number of epochs
  is defined by the `EBR_EPOCHS` constant and the epoch value is
  `0 <= epoch < EBR_EPOCHS`.
  * The synchronisation points must be serialised (e.g. if there are
  multiple G/C workers or other writers).  Generally, calls to
  `ebr_staging_epoch` and `ebr_gc_epoch` would be a part of the same
  serialised path.

* `unsigned ebr_staging_epoch(ebr_t *ebr)`
  * Returns an _epoch_ where objects can be staged for reclamation.
  This can be used as a reference value for the pending queue/tag, used
  to postpone the reclamation until this epoch becomes available for G/C.
  Note that this function would normally be serialised together with
  the `ebr_sync` calls.

* `unsigned ebr_gc_epoch(ebr_t *ebr)`
  * Returns the _epoch_ available for reclamation, i.e. the epoch where
  it is guaranteed that the objects are safe to be reclaimed/destroyed.
  The _epoch_ value will be the same as returned by the last successful
  `ebr_sync` call.  Note that these two functions would require the same
  form of serialisation.

* `void ebr_full_sync(ebr_t *ebr, unsigned msec_retry)`
  * Perform full synchronisation ensuring that all objects which are no
  longer globally visible (and potentially staged for reclamation) at the
  time of calling this routine will be safe to reclaim/destroy after this
  synchronisation routine completes and returns.  Note: the synchronisation
  may take across multiple epochs.
  * This function will block for `msec_retry` milliseconds before trying
  again if there are objects which cannot be reclaimed immediately.  If
  this value is zero, then it will invoke `sched_yield(2)` before retrying.

* `bool ebr_incrit_p(ebr_t *ebr)`
  * Returns `true` if the current worker is in the critical path, i.e.
  called `ebr_enter()`; otherwise, returns `false`.  This routine should
  generally only be used for diagnostic asserts.


## G/C API

* `gc_t *gc_create(unsigned entry_off, gc_func_t reclaim, void *arg)`
  * Construct a new G/C management object.  The `entry_off` argument is
  an offset of the `gc_entry_t` structure, which must be embedded in the
  object; typically, this value would be `offsetof(struct obj, gc_entry)`.
  The entry structure may also be embedded at the beginning of the object
  structure (offset being zero), should the caller need to support
  different object types.
  * A custom reclamation function can be used for object destruction.
  This function must process a list of objects, since a chain of objects
  may be passed for reclamation; the user can iterate the chain using
  the `gc_entry_t::next` member.  If _reclaim_ is NULL, then the default
  logic invoked by the G/C mechanism will be calling the system `free(3)`
  for each object.  An arbitrary user pointer, specified by `arg`, can
  be passed to the reclamation function.

* `void gc_destroy(gc_t *gc)`
  * Destroy the G/C management object.

* `void gc_register(gc_t *gc)`
  * Register the current thread as a user of the G/C mechanism.
  All threads having critical paths to reference the objects must register.

* `void gc_crit_enter(gc_t *gc)`
  * Enter the critical path where objects may be actively referenced.
  This prevents the G/C mechanism from reclaiming (destroying) the object.

* `void gc_crit_exit(gc_t *gc)`
  * Exit the critical path, indicating that the target objects no
  longer have active references and the G/C mechanism may consider
  them for reclamation.

* `void gc_limbo(gc_t *gc, void *obj)`
  * Insert the object into a "limbo" list, staging it for reclamation
  (destruction).  This is a request to reclaim the object once it is
  guaranteed that there are no threads referencing it in the critical path.

* `void gc_cycle(gc_t *gc)`
  * Run a G/C cycle attempting to reclaim some objects which were
  added to the limbo list.  The objects which are no longer referenced
  are not guaranteed to be reclaimed immediately after one cycle.  This
  function does not block and is expected to be called periodically for
  an incremental object reclamation.

* `void gc_full(gc_t *gc, unsigned msec_retry)`
  * Run a full G/C in order to ensure that all staged objects have been
  reclaimed.  This function will block for `msec_retry` milliseconds before
  trying again, if there are objects which cannot be reclaimed immediately.

## Notes

The implementation was extensively tested on a 24-core x86 machine,
see [the stress test](src/t_stress.c) for the details on the technique.

## Examples

### G/C API example

The G/C mechanism should be created by some master thread.
```c
typedef struct {
	...
	gc_entry_t	gc_entry;
} obj_t;

static gc_t *	gc;

void
some_sysinit(void)
{
	gc = gc_create(offsetof(obj_t, gc_entry), NULL, NULL);
	assert(gc != NULL);
	...
}
```

An example code fragment of a reader thread:
```c
	gc_register(gc);

	while (!exit) {
		/*
		 * Some processing which references the object(s).
		 * The readers must indicate the critical path where
		 * they actively reference objects.
		 */
		gc_crit_enter(gc);
		obj = object_lookup();
		process_object(obj);
		gc_crit_exit(gc);
	}
```

Here is an example code fragment in a writer thread which illustrates
how the object would be staged for destruction (reclamation):
```c
	/*
	 * Remove the object from the lock-free container.  The
	 * object is no longer globally visible.  Not it can be
	 * staged for destruction -- add it to the limbo list.
	 */
	obj = lockfree_remove(container, key);
	gc_limbo(gc, obj);
	...

	/*
	 * Checkpoint: run a G/C cycle attempting to reclaim *some*
	 * objects previously added to the limbo list.  This should be
	 * called periodically for incremental object reclamation.
	 *
	 * WARNING: All gc_cycle() calls must be serialised (using a
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

## Packages

Just build the package, install it and link the library using the
`-lqsbr` flag.
* RPM (tested on RHEL/CentOS 7): `cd pkg && make rpm`
* DEB (tested on Debian 9): `cd pkg && make deb`
