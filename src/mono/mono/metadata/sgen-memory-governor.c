/*
 * sgen-cardtable.c: Card table implementation for sgen
 *
 * Author:
 * 	Rodrigo Kumpera (rkumpera@novell.com)
 *
 * SGen is licensed under the terms of the MIT X11 license
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include "metadata/sgen-gc.h"
#include "metadata/sgen-memory-governor.h"
#include "metadata/mono-gc.h"

#include "utils/mono-counters.h"
#include "utils/mono-mmap.h"
#include "utils/mono-logger-internal.h"

#define MIN_MINOR_COLLECTION_ALLOWANCE	((mword)(DEFAULT_NURSERY_SIZE * SGEN_MIN_ALLOWANCE_NURSERY_SIZE_RATIO))

/*heap limits*/
static mword max_heap_size = ((mword)0)- ((mword)1);
static mword soft_heap_limit = ((mword)0) - ((mword)1);
static mword allocated_heap;

/*Memory usage tracking */
static mword total_alloc = 0;

/* GC triggers. */

/* use this to tune when to do a major/minor collection */
static mword memory_pressure = 0;
static mword minor_collection_allowance;
static int minor_collection_sections_alloced = 0;

static gboolean debug_print_allowance = FALSE;

/* GC stats */
static int last_major_num_sections = 0;
static int last_los_memory_usage = 0;
static gboolean major_collection_happened = FALSE;

static gboolean need_calculate_minor_collection_allowance;

static int last_collection_old_num_major_sections;
static mword last_collection_los_memory_usage = 0;
static mword last_collection_old_los_memory_usage;
static mword last_collection_los_memory_alloced;

static mword sgen_memgov_available_free_space (void);


/* GC trigger heuristics. */

static void
sgen_memgov_try_calculate_minor_collection_allowance (gboolean overwrite)
{
	int num_major_sections, num_major_sections_saved;
	mword los_memory_saved, new_major, new_heap_size, save_target, allowance_target;

	if (overwrite)
		g_assert (need_calculate_minor_collection_allowance);

	if (!need_calculate_minor_collection_allowance)
		return;

	if (!*major_collector.have_swept) {
		if (overwrite)
			minor_collection_allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
		return;
	}

	num_major_sections = major_collector.get_num_major_sections ();

	num_major_sections_saved = MAX (last_collection_old_num_major_sections - num_major_sections, 0);
	los_memory_saved = MAX (last_collection_old_los_memory_usage - last_collection_los_memory_usage, 1);

	new_major = num_major_sections * major_collector.section_size;
	new_heap_size = new_major + last_collection_los_memory_usage;

	save_target = (mword)((new_major + last_collection_los_memory_usage) * SGEN_DEFAULT_SAVE_TARGET_RATIO);

	/*
	 * We aim to allow the allocation of as many sections as is
	 * necessary to reclaim save_target sections in the next
	 * collection.  We assume the collection pattern won't change.
	 * In the last cycle, we had num_major_sections_saved for
	 * minor_collection_sections_alloced.  Assuming things won't
	 * change, this must be the same ratio as save_target for
	 * allowance_target, i.e.
	 *
	 *    num_major_sections_saved            save_target
	 * --------------------------------- == ----------------
	 * minor_collection_sections_alloced    allowance_target
	 *
	 * hence:
	 */
	allowance_target = (mword)((double)save_target * (double)(minor_collection_sections_alloced * major_collector.section_size + last_collection_los_memory_alloced) / (double)(num_major_sections_saved * major_collector.section_size + los_memory_saved));

	minor_collection_allowance = MAX (MIN (allowance_target, num_major_sections * major_collector.section_size + los_memory_usage), MIN_MINOR_COLLECTION_ALLOWANCE);

	if (new_heap_size + minor_collection_allowance > soft_heap_limit) {
		if (new_heap_size > soft_heap_limit)
			minor_collection_allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
		else
			minor_collection_allowance = MAX (soft_heap_limit - new_heap_size, MIN_MINOR_COLLECTION_ALLOWANCE);
	}

	if (debug_print_allowance) {
		mword old_major = last_collection_old_num_major_sections * major_collector.section_size;

		fprintf (gc_debug_file, "Before collection: %td bytes (%td major, %td LOS)\n",
				old_major + last_collection_old_los_memory_usage, old_major, last_collection_old_los_memory_usage);
		fprintf (gc_debug_file, "After collection: %td bytes (%td major, %td LOS)\n",
				new_heap_size, new_major, last_collection_los_memory_usage);
		fprintf (gc_debug_file, "Allowance: %td bytes\n", minor_collection_allowance);
	}

	if (major_collector.have_computed_minor_collection_allowance)
		major_collector.have_computed_minor_collection_allowance ();

	need_calculate_minor_collection_allowance = FALSE;
}


gboolean
sgen_need_major_collection (mword space_needed)
{
	mword los_alloced = los_memory_usage - MIN (last_collection_los_memory_usage, los_memory_usage);
	return (space_needed > sgen_memgov_available_free_space ()) ||
		minor_collection_sections_alloced * major_collector.section_size + los_alloced > minor_collection_allowance;
}

void
sgen_memgov_minor_collection_start (void)
{
	sgen_memgov_try_calculate_minor_collection_allowance (FALSE);
}

void
sgen_memgov_minor_collection_end (void)
{
}

void
sgen_memgov_major_collection_start (void)
{
	last_collection_old_num_major_sections = sgen_get_major_collector ()->get_num_major_sections ();

	/*
	 * A domain could have been freed, resulting in
	 * los_memory_usage being less than last_collection_los_memory_usage.
	 */
	last_collection_los_memory_alloced = los_memory_usage - MIN (last_collection_los_memory_usage, los_memory_usage);
	last_collection_old_los_memory_usage = los_memory_usage;

	need_calculate_minor_collection_allowance = TRUE;
	major_collection_happened = TRUE;
}

void
sgen_memgov_major_collection_end (void)
{
	sgen_memgov_try_calculate_minor_collection_allowance (TRUE);

	minor_collection_sections_alloced = 0;
	last_collection_los_memory_usage = los_memory_usage;
}

void
sgen_memgov_collection_start (int generation)
{
	last_major_num_sections = major_collector.get_num_major_sections ();
	last_los_memory_usage = los_memory_usage;
	major_collection_happened = FALSE;
}

void
sgen_memgov_collection_end (int generation, unsigned long pause_time, unsigned long bridge_pause_time)
{
	int num_major_sections = major_collector.get_num_major_sections ();

	if (major_collection_happened)
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MAJOR: %s pause %.2fms, bridge %.2fms major %dK/%dK los %dK/%dK",
			generation ? "" : "(minor overflow)",
			(int)pause_time / 1000.0f, (int)bridge_pause_time / 1000.0f,
			major_collector.section_size * num_major_sections / 1024,
			major_collector.section_size * last_major_num_sections / 1024,
			los_memory_usage / 1024,
			last_los_memory_usage / 1024);
	else
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MINOR: pause %.2fms, bridge %.2fms promoted %dK major %dK los %dK",
			(int)pause_time / 1000.0f, (int)bridge_pause_time / 1000.0f,
			(num_major_sections - last_major_num_sections) * major_collector.section_size / 1024,
			major_collector.section_size * num_major_sections / 1024,
			los_memory_usage / 1024);
}
void
sgen_register_major_sections_alloced (int num_sections)
{
	minor_collection_sections_alloced += num_sections;
}

mword
sgen_get_minor_collection_allowance (void)
{
	return minor_collection_allowance;
}

/* Memory pressure API */

/* Negative value to remove */
void
mono_gc_add_memory_pressure (gint64 value)
{
	/* FIXME: Use interlocked functions */
	LOCK_GC;
	memory_pressure += value;
	UNLOCK_GC;
}


/*
Global GC memory tracking.
This tracks the total usage of memory by the GC. This includes
managed and unmanaged memory.
*/

static unsigned long
prot_flags_for_activate (int activate)
{
	unsigned long prot_flags = activate? MONO_MMAP_READ|MONO_MMAP_WRITE: MONO_MMAP_NONE;
	return prot_flags | MONO_MMAP_PRIVATE | MONO_MMAP_ANON;
}

/*
 * Allocate a big chunk of memory from the OS (usually 64KB to several megabytes).
 * This must not require any lock.
 */
void*
sgen_alloc_os_memory (size_t size, int activate)
{
	void *ptr = mono_valloc (0, size, prot_flags_for_activate (activate));
	if (ptr)
		SGEN_ATOMIC_ADD_P (total_alloc, size);
	return ptr;
}

/* size must be a power of 2 */
void*
sgen_alloc_os_memory_aligned (size_t size, mword alignment, gboolean activate)
{
	void *ptr = mono_valloc_aligned (size, alignment, prot_flags_for_activate (activate));
	if (ptr)
		SGEN_ATOMIC_ADD_P (total_alloc, size);
	return ptr;
}

/*
 * Free the memory returned by sgen_alloc_os_memory (), returning it to the OS.
 */
void
sgen_free_os_memory (void *addr, size_t size)
{
	mono_vfree (addr, size);
	SGEN_ATOMIC_ADD_P (total_alloc, -size);
}

int64_t
mono_gc_get_heap_size (void)
{
	return total_alloc;
}


/*
Heap Sizing limits.
This limit the max size of the heap. It takes into account
only memory actively in use to hold heap objects and not
for other parts of the GC.
 */
static mword
sgen_memgov_available_free_space (void)
{
	return max_heap_size - MIN (allocated_heap, max_heap_size);
}

void
sgen_memgov_release_space (mword size, int space)
{
	SGEN_ATOMIC_ADD_P (allocated_heap, -size);
}

gboolean
sgen_memgov_try_alloc_space (mword size, int space)
{
	if (sgen_memgov_available_free_space () < size)
		return FALSE;

	SGEN_ATOMIC_ADD_P (allocated_heap, size);
	mono_runtime_resource_check_limit (MONO_RESOURCE_GC_HEAP, allocated_heap);
	return TRUE;
}

void
sgen_memgov_init (glong max_heap, glong soft_limit, gboolean debug_allowance)
{
	if (soft_limit)
		soft_heap_limit = soft_limit;

	debug_print_allowance = debug_allowance;

	if (max_heap == 0)
		return;

	if (max_heap < soft_limit) {
		fprintf (stderr, "max-heap-size must be at least as large as soft-heap-limit.\n");
		exit (1);
	}

	if (max_heap < sgen_nursery_size * 4) {
		fprintf (stderr, "max-heap-size must be at least 4 times larger than nursery size.\n");
		exit (1);
	}
	max_heap_size = max_heap - sgen_nursery_size;

	minor_collection_allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
}

#endif
