/*
 * sgen-major-copying.c: Simple generational GC.
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *
 * Copyright 2005-2010 Novell, Inc (http://www.novell.com)
 *
 * Thread start/stop adapted from Boehm's GC:
 * Copyright (c) 1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1998 by Fergus Henderson.  All rights reserved.
 * Copyright (c) 2000-2004 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose,  provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 *
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
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

#define MAJOR_SECTION_SIZE		PINNED_CHUNK_SIZE
#define BLOCK_FOR_OBJECT(o)		((Block*)(((mword)(o)) & ~(MAJOR_SECTION_SIZE - 1)))
#define MAJOR_SECTION_FOR_OBJECT(o)	((GCMemSection*)BLOCK_FOR_OBJECT ((o)))
#define MIN_MINOR_COLLECTION_SECTION_ALLOWANCE	(DEFAULT_NURSERY_SIZE * 3 / MAJOR_SECTION_SIZE)

#define MAJOR_OBJ_IS_IN_TO_SPACE(o)	(MAJOR_SECTION_FOR_OBJECT ((o))->is_to_space)

static int minor_collection_section_allowance;
static int minor_collection_sections_alloced = 0;
static int num_major_sections = 0;

static GCMemSection *section_list = NULL;

/* pinned_chunk_list is used for allocations of objects that are never moved */
static PinnedChunk *pinned_chunk_list = NULL;

/*
 * used when moving the objects
 */
static char *to_space_bumper = NULL;
static char *to_space_top = NULL;
static GCMemSection *to_space_section = NULL;

static gboolean
obj_is_from_pinned_alloc (char *p)
{
	return BLOCK_FOR_OBJECT (p)->role == MEMORY_ROLE_PINNED;
}

static void
free_pinned_object (char *obj, size_t size)
{
	PinnedChunk *chunk = (PinnedChunk*) BLOCK_FOR_OBJECT (obj);
	void **p = (void**)obj;
	int slot = slot_for_size (size);

	g_assert (obj >= (char*)chunk->start_data && obj < ((char*)chunk + chunk->num_pages * FREELIST_PAGESIZE));
	*p = chunk->free_list [slot];
	chunk->free_list [slot] = p;
}

/*
 * Allocate a new section of memory to be used as old generation.
 */
static GCMemSection*
alloc_major_section (void)
{
	GCMemSection *section;
	int scan_starts;

	section = get_os_memory_aligned (MAJOR_SECTION_SIZE, MAJOR_SECTION_SIZE, TRUE);
	section->next_data = section->data = (char*)section + SIZEOF_GC_MEM_SECTION;
	g_assert (!((mword)section->data & 7));
	section->size = MAJOR_SECTION_SIZE - SIZEOF_GC_MEM_SECTION;
	section->end_data = section->data + section->size;
	UPDATE_HEAP_BOUNDARIES (section->data, section->end_data);
	total_alloc += section->size;
	DEBUG (3, fprintf (gc_debug_file, "New major heap section: (%p-%p), total: %zd\n", section->data, section->end_data, total_alloc));
	scan_starts = (section->size + SCAN_START_SIZE - 1) / SCAN_START_SIZE;
	section->scan_starts = get_internal_mem (sizeof (char*) * scan_starts, INTERNAL_MEM_SCAN_STARTS);
	section->num_scan_start = scan_starts;
	section->block.role = MEMORY_ROLE_GEN1;
	section->is_to_space = TRUE;

	/* add to the section list */
	section->block.next = section_list;
	section_list = section;

	++num_major_sections;

	return section;
}

static void
free_major_section (GCMemSection *section)
{
	DEBUG (3, fprintf (gc_debug_file, "Freed major section %p (%p-%p)\n", section, section->data, section->end_data));
	free_internal_mem (section->scan_starts, INTERNAL_MEM_SCAN_STARTS);
	free_os_memory (section, MAJOR_SECTION_SIZE);
	total_alloc -= MAJOR_SECTION_SIZE - SIZEOF_GC_MEM_SECTION;

	--num_major_sections;
}

static void
new_to_space_section (void)
{
	/* FIXME: if the current to_space_section is empty, we don't
	   have to allocate a new one */

	to_space_section = alloc_major_section ();
	to_space_bumper = to_space_section->next_data;
	to_space_top = to_space_section->end_data;
}

static void
to_space_set_next_data (void)
{
	g_assert (to_space_bumper >= to_space_section->next_data && to_space_bumper <= to_space_section->end_data);
	to_space_section->next_data = to_space_bumper;
}

static void
to_space_expand (void)
{
	if (to_space_section) {
		g_assert (to_space_top == to_space_section->end_data);
		to_space_set_next_data ();
	}

	new_to_space_section ();
}

#define MAJOR_GET_COPY_OBJECT_SPACE(dest, size) do {			\
		(dest) = to_space_bumper;				\
		/* Make sure we have enough space available */		\
		if ((dest) + (size) > to_space_top) {			\
			to_space_expand ();				\
			(dest) = to_space_bumper;			\
			DEBUG (8, g_assert ((dest) + (objsize) <= to_space_top)); \
		}							\
		to_space_bumper += objsize;				\
		DEBUG (8, g_assert (to_space_bumper <= to_space_top));	\
		to_space_section->scan_starts [((dest) - (char*)to_space_section->data)/SCAN_START_SIZE] = (dest); \
	} while (0)

static void
unset_to_space (void)
{
	/* between collections the to_space_bumper is invalidated
	   because degraded allocations might occur, so we set it to
	   NULL, just to make it explicit */
	to_space_bumper = NULL;

	/* don't unset to_space_section if we implement the FIXME in
	   new_to_space_section */
	to_space_section = NULL;
}

static gboolean
major_is_object_live (char *obj)
{
	mword objsize;

	/* nursery */
	if (ptr_in_nursery (obj))
		return FALSE;

	objsize = safe_object_get_size ((MonoObject*)obj);
	objsize += ALLOC_ALIGN - 1;
	objsize &= ~(ALLOC_ALIGN - 1);

	/* LOS */
	if (objsize > MAX_SMALL_OBJ_SIZE)
		return FALSE;

	/* pinned chunk */
	if (obj_is_from_pinned_alloc (obj))
		return FALSE;

	/* now we know it's in a major heap section */
	return MAJOR_SECTION_FOR_OBJECT (obj)->is_to_space;
}

/* size is a multiple of ALLOC_ALIGN */
static void*
major_alloc_small_pinned_obj (size_t size)
{
	int slot;
	void *res = NULL;
	PinnedChunk *pchunk;
	slot = slot_for_size (size);
	/*g_print ("using slot %d for size %d (slot size: %d)\n", slot, size, freelist_sizes [slot]);*/
	g_assert (size <= freelist_sizes [slot]);
	for (pchunk = pinned_chunk_list; pchunk; pchunk = pchunk->block.next) {
		void **p = pchunk->free_list [slot];
		if (p) {
			/*g_print ("found freelist for slot %d in chunk %p, returning %p, next %p\n", slot, pchunk, p, *p);*/
			pchunk->free_list [slot] = *p;
			res = p;
			goto found;
		}
	}
	for (pchunk = pinned_chunk_list; pchunk; pchunk = pchunk->block.next) {
		res = get_chunk_freelist (pchunk, slot);
		if (res)
			goto found;
	}
	pchunk = alloc_pinned_chunk ();
	/* FIXME: handle OOM */
	pchunk->block.next = pinned_chunk_list;
	pinned_chunk_list = pchunk;
	res = get_chunk_freelist (pchunk, slot);
 found:
	memset (res, 0, size);
	return res;
}

/*
 * size is already rounded up and we hold the GC lock.
 */
static void*
alloc_degraded (MonoVTable *vtable, size_t size)
{
	GCMemSection *section;
	void **p = NULL;
	g_assert (size <= MAX_SMALL_OBJ_SIZE);
	HEAVY_STAT (++stat_objects_alloced_degraded);
	HEAVY_STAT (stat_bytes_alloced_degraded += size);
	for (section = section_list; section; section = section->block.next) {
		if ((section->end_data - section->next_data) >= size) {
			p = (void**)section->next_data;
			break;
		}
	}
	if (!p) {
		section = alloc_major_section ();
		section->is_to_space = FALSE;
		/* FIXME: handle OOM */
		p = (void**)section->next_data;
	}
	section->next_data += size;
	degraded_mode += size;
	DEBUG (3, fprintf (gc_debug_file, "Allocated (degraded) object %p, vtable: %p (%s), size: %zd in section %p\n", p, vtable, vtable->klass->name, size, section));
	*p = vtable;
	return p;
}

static void
major_copy_or_mark_object (void **obj_slot)
{
	char *forwarded;
	char *obj = *obj_slot;
	mword objsize;

	DEBUG (9, g_assert (current_collection_generation == GENERATION_OLD));

	HEAVY_STAT (++num_copy_object_called);

	DEBUG (9, fprintf (gc_debug_file, "Precise copy of %p from %p", obj, obj_slot));

	/*
	 * obj must belong to one of:
	 *
	 * 1. the nursery
	 * 2. the LOS
	 * 3. a pinned chunk
	 * 4. a non-to-space section of the major heap
	 * 5. a to-space section of the major heap
	 *
	 * In addition, objects in 1, 2 and 4 might also be pinned.
	 * Objects in 1 and 4 might be forwarded.
	 *
	 * Before we can copy the object we must make sure that we are
	 * allowed to, i.e. that the object not pinned, not already
	 * forwarded and doesn't belong to the LOS, a pinned chunk, or
	 * a to-space section.
	 *
	 * We are usually called for to-space objects (5) when we have
	 * two remset entries for the same reference.  The first entry
	 * copies the object and updates the reference and the second
	 * calls us with the updated reference that points into
	 * to-space.  There might also be other circumstances where we
	 * get to-space objects.
	 */

	if ((forwarded = object_is_forwarded (obj))) {
		DEBUG (9, g_assert (((MonoVTable*)LOAD_VTABLE(obj))->gc_descr));
		DEBUG (9, fprintf (gc_debug_file, " (already forwarded to %p)\n", forwarded));
		HEAVY_STAT (++stat_copy_object_failed_forwarded);
		*obj_slot = forwarded;
		return;
	}
	if (object_is_pinned (obj)) {
		DEBUG (9, g_assert (((MonoVTable*)LOAD_VTABLE(obj))->gc_descr));
		DEBUG (9, fprintf (gc_debug_file, " (pinned, no change)\n"));
		HEAVY_STAT (++stat_copy_object_failed_pinned);
		return;
	}

	if (ptr_in_nursery (obj))
		goto copy;

	/*
	 * At this point we know obj is not pinned, not forwarded and
	 * belongs to 2, 3, 4, or 5.
	 *
	 * LOS object (2) are simple, at least until we always follow
	 * the rule: if objsize > MAX_SMALL_OBJ_SIZE, pin the object
	 * and return it.  At the end of major collections, we walk
	 * the los list and if the object is pinned, it is marked,
	 * otherwise it can be freed.
	 *
	 * Pinned chunks (3) and major heap sections (4, 5) both
	 * reside in blocks, which are always aligned, so once we've
	 * eliminated LOS objects, we can just access the block and
	 * see whether it's a pinned chunk or a major heap section.
	 */

	objsize = safe_object_get_size ((MonoObject*)obj);
	objsize += ALLOC_ALIGN - 1;
	objsize &= ~(ALLOC_ALIGN - 1);

	if (G_UNLIKELY (objsize > MAX_SMALL_OBJ_SIZE || obj_is_from_pinned_alloc (obj))) {
		if (object_is_pinned (obj))
			return;
		DEBUG (9, fprintf (gc_debug_file, " (marked LOS/Pinned %p (%s), size: %zd)\n", obj, safe_name (obj), objsize));
		binary_protocol_pin (obj, (gpointer)LOAD_VTABLE (obj), safe_object_get_size ((MonoObject*)obj));
		pin_object (obj);
		GRAY_OBJECT_ENQUEUE (obj);
		HEAVY_STAT (++stat_copy_object_failed_large_pinned);
		return;
	}

	/*
	 * Now we know the object is in a major heap section.  All we
	 * need to do is check whether it's already in to-space (5) or
	 * not (4).
	 */
	if (MAJOR_OBJ_IS_IN_TO_SPACE (obj)) {
		DEBUG (9, g_assert (objsize <= MAX_SMALL_OBJ_SIZE));
		DEBUG (9, fprintf (gc_debug_file, " (already copied)\n"));
		HEAVY_STAT (++stat_copy_object_failed_to_space);
		return;
	}

 copy:
	*obj_slot = copy_object_no_checks (obj);
}

/* FIXME: later reduce code duplication here with build_nursery_fragments().
 * We don't keep track of section fragments for non-nursery sections yet, so
 * just memset to 0.
 */
static void
build_section_fragments (GCMemSection *section)
{
	int i;
	char *frag_start, *frag_end;
	size_t frag_size;

	/* clear scan starts */
	memset (section->scan_starts, 0, section->num_scan_start * sizeof (gpointer));
	frag_start = section->data;
	section->next_data = section->data;
	for (i = section->pin_queue_start; i < section->pin_queue_end; ++i) {
		frag_end = pin_queue [i];
		/* remove the pin bit from pinned objects */
		unpin_object (frag_end);
		if (frag_end >= section->data + section->size) {
			frag_end = section->data + section->size;
		} else {
			section->scan_starts [((char*)frag_end - (char*)section->data)/SCAN_START_SIZE] = frag_end;
		}
		frag_size = frag_end - frag_start;
		if (frag_size) {
			binary_protocol_empty (frag_start, frag_size);
			memset (frag_start, 0, frag_size);
		}
		frag_size = safe_object_get_size ((MonoObject*)pin_queue [i]);
		frag_size += ALLOC_ALIGN - 1;
		frag_size &= ~(ALLOC_ALIGN - 1);
		frag_start = (char*)pin_queue [i] + frag_size;
		section->next_data = MAX (section->next_data, frag_start);
	}
	frag_end = section->end_data;
	frag_size = frag_end - frag_start;
	if (frag_size) {
		binary_protocol_empty (frag_start, frag_size);
		memset (frag_start, 0, frag_size);
	}
}

static void
scan_pinned_objects (IterateObjectCallbackFunc callback, void *callback_data)
{
	PinnedChunk *chunk;
	int i, obj_size;
	char *p, *endp;
	void **ptr;
	void *end_chunk;
	for (chunk = pinned_chunk_list; chunk; chunk = chunk->block.next) {
		end_chunk = (char*)chunk + chunk->num_pages * FREELIST_PAGESIZE;
		DEBUG (6, fprintf (gc_debug_file, "Scanning pinned chunk %p (range: %p-%p)\n", chunk, chunk->start_data, end_chunk));
		for (i = 0; i < chunk->num_pages; ++i) {
			obj_size = chunk->page_sizes [i];
			if (!obj_size)
				continue;
			p = i? (char*)chunk + i * FREELIST_PAGESIZE: chunk->start_data;
			endp = i? p + FREELIST_PAGESIZE: (char*)chunk + FREELIST_PAGESIZE;
			DEBUG (6, fprintf (gc_debug_file, "Page %d (size: %d, range: %p-%p)\n", i, obj_size, p, endp));
			while (p + obj_size <= endp) {
				ptr = (void**)p;
				DEBUG (9, fprintf (gc_debug_file, "Considering %p (vtable: %p)\n", ptr, *ptr));
				/* if the first word (the vtable) is outside the chunk we have an object */
				if (*ptr && (*ptr < (void*)chunk || *ptr >= end_chunk))
					callback ((char*)ptr, obj_size, callback_data);
				p += obj_size;
			}
		}
	}
}

/*
 * the array of pointers from @start to @end contains conservative
 * pointers to objects inside @chunk: mark each referenced object
 * with the PIN bit.
 */
static void
mark_pinned_from_addresses (PinnedChunk *chunk, void **start, void **end)
{
	for (; start < end; start++) {
		char *addr = *start;
		int offset = (char*)addr - (char*)chunk;
		int page = offset / FREELIST_PAGESIZE;
		int obj_offset = page == 0? offset - ((char*)chunk->start_data - (char*)chunk): offset % FREELIST_PAGESIZE;
		int slot_size = chunk->page_sizes [page];
		void **ptr;
		/* the page is not allocated */
		if (!slot_size)
			continue;
		/* would be faster if we restrict the sizes to power of two,
		 * but that's a waste of memory: need to measure. it could reduce
		 * fragmentation since there are less pages needed, if for example
		 * someone interns strings of each size we end up with one page per
		 * interned string (still this is just ~40 KB): with more fine-grained sizes
		 * this increases the number of used pages.
		 */
		if (page == 0) {
			obj_offset /= slot_size;
			obj_offset *= slot_size;
			addr = (char*)chunk->start_data + obj_offset;
		} else {
			obj_offset /= slot_size;
			obj_offset *= slot_size;
			addr = (char*)chunk + page * FREELIST_PAGESIZE + obj_offset;
		}
		ptr = (void**)addr;
		/* if the vtable is inside the chunk it's on the freelist, so skip */
		if (*ptr && (*ptr < (void*)chunk->start_data || *ptr > (void*)((char*)chunk + chunk->num_pages * FREELIST_PAGESIZE))) {
			binary_protocol_pin (addr, (gpointer)LOAD_VTABLE (addr), safe_object_get_size ((MonoObject*)addr));
			if (heap_dump_file && !object_is_pinned (addr))
				pin_stats_register_object ((char*) addr, safe_object_get_size ((MonoObject*) addr));
			pin_object (addr);
			GRAY_OBJECT_ENQUEUE (addr);
			DEBUG (6, fprintf (gc_debug_file, "Marked pinned object %p (%s) from roots\n", addr, safe_name (addr)));
		}
	}
}

static void
sweep_pinned_objects_callback (char *ptr, size_t size, void *data)
{
	if (object_is_pinned (ptr)) {
		unpin_object (ptr);
		DEBUG (6, fprintf (gc_debug_file, "Unmarked pinned object %p (%s)\n", ptr, safe_name (ptr)));
	} else {
		DEBUG (6, fprintf (gc_debug_file, "Freeing unmarked pinned object %p (%s)\n", ptr, safe_name (ptr)));
		free_pinned_object (ptr, size);
	}
}

static void
sweep_pinned_objects (void)
{
	scan_pinned_objects (sweep_pinned_objects_callback, NULL);
}

static void
major_iterate_objects (gboolean non_pinned, gboolean pinned, IterateObjectCallbackFunc callback, void *data)
{
	if (non_pinned) {
		GCMemSection *section;
		for (section = section_list; section; section = section->block.next)
			scan_area_with_callback (section->data, section->end_data, callback, data);
	}
	if (pinned)
		scan_pinned_objects (callback, data);
}

static void
major_free_non_pinned_object (char *obj, size_t size)
{
	memset (obj, 0, size);
}

static void
major_do_collection (const char *reason)
{
	GCMemSection *section, *prev_section;
	LOSObject *bigobj, *prevbo;
	PinnedChunk *chunk;
	TV_DECLARE (all_atv);
	TV_DECLARE (all_btv);
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	/* FIXME: only use these values for the precise scan
	 * note that to_space pointers should be excluded anyway...
	 */
	char *heap_start = NULL;
	char *heap_end = (char*)-1;
	int old_num_major_sections = num_major_sections;
	int num_major_sections_saved, save_target, allowance_target;

	init_stats ();
	binary_protocol_collection (GENERATION_OLD);
	check_scan_starts ();
	gray_object_queue_init ();

	degraded_mode = 0;
	DEBUG (1, fprintf (gc_debug_file, "Start major collection %d\n", num_major_gcs));
	num_major_gcs++;
	mono_stats.major_gc_count ++;

	/* world must be stopped already */
	TV_GETTIME (all_atv);
	TV_GETTIME (atv);

	/* Pinning depends on this */
	clear_nursery_fragments (nursery_next);

	TV_GETTIME (btv);
	time_major_pre_collection_fragment_clear += TV_ELAPSED_MS (atv, btv);

	if (xdomain_checks)
		check_for_xdomain_refs ();

	nursery_section->next_data = nursery_real_end;
	/* we should also coalesce scanning from sections close to each other
	 * and deal with pointers outside of the sections later.
	 */
	/* The remsets are not useful for a major collection */
	clear_remsets ();

	TV_GETTIME (atv);
	init_pinning ();
	DEBUG (6, fprintf (gc_debug_file, "Collecting pinned addresses\n"));
	pin_from_roots ((void*)lowest_heap_address, (void*)highest_heap_address);
	optimize_pin_queue (0);

	/*
	 * pin_queue now contains all candidate pointers, sorted and
	 * uniqued.  We must do two passes now to figure out which
	 * objects are pinned.
	 *
	 * The first is to find within the pin_queue the area for each
	 * section.  This requires that the pin_queue be sorted.  We
	 * also process the LOS objects and pinned chunks here.
	 *
	 * The second, destructive, pass is to reduce the section
	 * areas to pointers to the actually pinned objects.
	 */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from sections\n"));
	/* first pass for the sections */
	find_section_pin_queue_start_end (nursery_section);
	for (section = section_list; section; section = section->block.next)
		find_section_pin_queue_start_end (section);
	/* identify possible pointers to the insize of large objects */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from large objects\n"));
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		int start, end;
		find_optimized_pin_queue_area (bigobj->data, (char*)bigobj->data + bigobj->size, &start, &end);
		if (start != end) {
			pin_object (bigobj->data);
			GRAY_OBJECT_ENQUEUE (bigobj->data);
			if (heap_dump_file)
				pin_stats_register_object ((char*) bigobj->data, safe_object_get_size ((MonoObject*) bigobj->data));
			DEBUG (6, fprintf (gc_debug_file, "Marked large object %p (%s) size: %zd from roots\n", bigobj->data, safe_name (bigobj->data), bigobj->size));
		}
	}
	/* look for pinned addresses for pinned-alloc objects */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from pinned-alloc objects\n"));
	for (chunk = pinned_chunk_list; chunk; chunk = chunk->block.next) {
		int start, end;
		find_optimized_pin_queue_area (chunk->start_data, (char*)chunk + chunk->num_pages * FREELIST_PAGESIZE, &start, &end);
		if (start != end)
			mark_pinned_from_addresses (chunk, pin_queue + start, pin_queue + end);
	}
	/* second pass for the sections */
	pin_objects_in_section (nursery_section);
	for (section = section_list; section; section = section->block.next)
		pin_objects_in_section (section);

	TV_GETTIME (btv);
	time_major_pinning += TV_ELAPSED_MS (atv, btv);
	DEBUG (2, fprintf (gc_debug_file, "Finding pinned pointers: %d in %d usecs\n", next_pin_slot, TV_ELAPSED (atv, btv)));
	DEBUG (4, fprintf (gc_debug_file, "Start scan with %d pinned objects\n", next_pin_slot));

	new_to_space_section ();

	drain_gray_stack ();

	TV_GETTIME (atv);
	time_major_scan_pinned += TV_ELAPSED_MS (btv, atv);

	/* registered roots, this includes static fields */
	scan_from_registered_roots (major_copy_or_mark_object, heap_start, heap_end, ROOT_TYPE_NORMAL);
	scan_from_registered_roots (major_copy_or_mark_object, heap_start, heap_end, ROOT_TYPE_WBARRIER);
	TV_GETTIME (btv);
	time_major_scan_registered_roots += TV_ELAPSED_MS (atv, btv);

	/* Threads */
	/* FIXME: this is the wrong place for this, because it does
	   pinning */
	scan_thread_data (heap_start, heap_end, TRUE);
	TV_GETTIME (atv);
	time_major_scan_thread_data += TV_ELAPSED_MS (btv, atv);

	TV_GETTIME (btv);
	time_major_scan_alloc_pinned += TV_ELAPSED_MS (atv, btv);

	/* scan the list of objects ready for finalization */
	scan_finalizer_entries (major_copy_or_mark_object, fin_ready_list);
	scan_finalizer_entries (major_copy_or_mark_object, critical_fin_list);
	TV_GETTIME (atv);
	time_major_scan_finalized += TV_ELAPSED_MS (btv, atv);
	DEBUG (2, fprintf (gc_debug_file, "Root scan: %d usecs\n", TV_ELAPSED (btv, atv)));

	TV_GETTIME (btv);
	time_major_scan_big_objects += TV_ELAPSED_MS (atv, btv);

	/* all the objects in the heap */
	finish_gray_stack (heap_start, heap_end, GENERATION_OLD);
	TV_GETTIME (atv);
	time_major_finish_gray_stack += TV_ELAPSED_MS (btv, atv);

	to_space_set_next_data ();
	unset_to_space ();

	/* sweep the big objects list */
	prevbo = NULL;
	for (bigobj = los_object_list; bigobj;) {
		if (object_is_pinned (bigobj->data)) {
			unpin_object (bigobj->data);
		} else {
			LOSObject *to_free;
			/* not referenced anywhere, so we can free it */
			if (prevbo)
				prevbo->next = bigobj->next;
			else
				los_object_list = bigobj->next;
			to_free = bigobj;
			bigobj = bigobj->next;
			free_large_object (to_free);
			continue;
		}
		prevbo = bigobj;
		bigobj = bigobj->next;
	}
	/* unpin objects from the pinned chunks and free the unmarked ones */
	sweep_pinned_objects ();

	TV_GETTIME (btv);
	time_major_sweep += TV_ELAPSED_MS (atv, btv);

	/* free the unused sections */
	prev_section = NULL;
	for (section = section_list; section;) {
		/* to_space doesn't need handling here */
		if (section->is_to_space) {
			section->is_to_space = FALSE;
			prev_section = section;
			section = section->block.next;
			continue;
		}
		/* no pinning object, so the section is free */
		if (section->pin_queue_start == section->pin_queue_end) {
			GCMemSection *to_free;
			if (prev_section)
				prev_section->block.next = section->block.next;
			else
				section_list = section->block.next;
			to_free = section;
			section = section->block.next;
			free_major_section (to_free);
			continue;
		} else {
			DEBUG (6, fprintf (gc_debug_file, "Section %p has still pinned objects (%d)\n", section, section->pin_queue_end - section->pin_queue_start));
			build_section_fragments (section);
		}
		prev_section = section;
		section = section->block.next;
	}

	/* walk the pin_queue, build up the fragment list of free memory, unmark
	 * pinned objects as we go, memzero() the empty fragments so they are ready for the
	 * next allocations.
	 */
	build_nursery_fragments (nursery_section->pin_queue_start, nursery_section->pin_queue_end);

	TV_GETTIME (atv);
	time_major_fragment_creation += TV_ELAPSED_MS (btv, atv);

	TV_GETTIME (all_btv);
	mono_stats.major_gc_time_usecs += TV_ELAPSED (all_atv, all_btv);

	if (heap_dump_file)
		dump_heap ("major", num_major_gcs - 1, reason);

	/* prepare the pin queue for the next collection */
	next_pin_slot = 0;
	if (fin_ready_list || critical_fin_list) {
		DEBUG (4, fprintf (gc_debug_file, "Finalizer-thread wakeup: ready %d\n", num_ready_finalizers));
		mono_gc_finalize_notify ();
	}
	pin_stats_reset ();

	g_assert (gray_object_queue_is_empty ());

	commit_stats (GENERATION_OLD);

	num_major_sections_saved = MAX (old_num_major_sections - num_major_sections, 1);

	save_target = num_major_sections / 2;
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
	allowance_target = save_target * minor_collection_sections_alloced / num_major_sections_saved;

	minor_collection_section_allowance = MAX (MIN (allowance_target, num_major_sections), MIN_MINOR_COLLECTION_SECTION_ALLOWANCE);

	minor_collection_sections_alloced = 0;

	check_scan_starts ();
}

static void
major_check_scan_starts (void)
{
	GCMemSection *section;
	for (section = section_list; section; section = section->block.next)
		check_section_scan_starts (section);
}

static void
major_dump_non_pinned_sections (void)
{
	GCMemSection *section;
	for (section = section_list; section; section = section->block.next)
		dump_section (section, "old");
}

static gint64
major_get_used_size (void)
{
	gint64 tot = 0;
	GCMemSection *section;
	for (section = section_list; section; section = section->block.next) {
		/* this is approximate... */
		tot += section->next_data - section->data;
	}
	return tot;
}

static void
major_init (void)
{
	minor_collection_section_allowance = MIN_MINOR_COLLECTION_SECTION_ALLOWANCE;
}

/* only valid during minor collections */
static int old_num_major_sections;

static void
major_start_nursery_collection (void)
{
	old_num_major_sections = num_major_sections;

	if (!to_space_section) {
		new_to_space_section ();
	} else {
		/* we might have done degraded allocation since the
		   last collection */
		g_assert (to_space_bumper <= to_space_section->next_data);
		to_space_bumper = to_space_section->next_data;

		to_space_section->is_to_space = TRUE;
	}
}

static void
major_finish_nursery_collection (void)
{
	GCMemSection *section;
	int sections_alloced;

	to_space_set_next_data ();

	for (section = section_list; section; section = section->block.next)
		section->is_to_space = FALSE;

	sections_alloced = num_major_sections - old_num_major_sections;
	minor_collection_sections_alloced += sections_alloced;
}

static gboolean
major_need_major_collection (void)
{
	return minor_collection_sections_alloced > minor_collection_section_allowance;
}

static gboolean
major_ptr_is_in_non_pinned_space (char *ptr)
{
	GCMemSection *section;
	for (section = section_list; section;) {
		if (ptr >= section->data && ptr < section->data + section->size)
			return TRUE;
		section = section->block.next;
	}
	return FALSE;
}

static void
major_report_pinned_memory_usage (void)
{
	PinnedChunk *chunk;
	int i = 0;
	for (chunk = pinned_chunk_list; chunk; chunk = chunk->block.next)
		report_pinned_chunk (chunk, i++);
}
