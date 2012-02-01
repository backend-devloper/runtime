/*
 * sgen-debug.c: Collector debugging
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *  Rodrigo Kumpera (kumpera@gmail.com)
 *
 * Copyright 2005-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Copyright 2011 Xamarin, Inc.
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
#include "metadata/sgen-cardtable.h"
#include "metadata/sgen-ssb.h"
#include "metadata/sgen-protocol.h"

#define LOAD_VTABLE	SGEN_LOAD_VTABLE

#define object_is_forwarded	SGEN_OBJECT_IS_FORWARDED
#define object_is_pinned	SGEN_OBJECT_IS_PINNED
#define safe_object_get_size	mono_sgen_safe_object_get_size

void describe_ptr (char *ptr);
void check_object (char *start);

/*
 * ######################################################################
 * ########  Collector debugging
 * ######################################################################
 */

const char*descriptor_types [] = {
	"run_length",
	"small_bitmap",
	"string",
	"complex",
	"vector",
	"array",
	"large_bitmap",
	"complex_arr"
};

void
describe_ptr (char *ptr)
{
	MonoVTable *vtable;
	mword desc;
	int type;
	char *start;

	if (mono_sgen_ptr_in_nursery (ptr)) {
		printf ("Pointer inside nursery.\n");
	} else {
		if (mono_sgen_ptr_is_in_los (ptr, &start)) {
			if (ptr == start)
				printf ("Pointer is the start of object %p in LOS space.\n", start);
			else
				printf ("Pointer is at offset 0x%x of object %p in LOS space.\n", (int)(ptr - start), start);
			ptr = start;
		} else if (major_collector.ptr_is_in_non_pinned_space (ptr)) {
			printf ("Pointer inside oldspace.\n");
		} else if (major_collector.obj_is_from_pinned_alloc (ptr)) {
			printf ("Pointer is inside a pinned chunk.\n");
		} else {
			printf ("Pointer unknown.\n");
			return;
		}
	}

	if (object_is_pinned (ptr))
		printf ("Object is pinned.\n");

	if (object_is_forwarded (ptr))
		printf ("Object is forwared.\n");

	// FIXME: Handle pointers to the inside of objects
	vtable = (MonoVTable*)LOAD_VTABLE (ptr);

	printf ("VTable: %p\n", vtable);
	if (vtable == NULL) {
		printf ("VTable is invalid (empty).\n");
		return;
	}
	if (mono_sgen_ptr_in_nursery (vtable)) {
		printf ("VTable is invalid (points inside nursery).\n");
		return;
	}
	printf ("Class: %s\n", vtable->klass->name);

	desc = ((GCVTable*)vtable)->desc;
	printf ("Descriptor: %lx\n", (long)desc);

	type = desc & 0x7;
	printf ("Descriptor type: %d (%s)\n", type, descriptor_types [type]);
}

static gboolean missing_remsets;

/*
 * We let a missing remset slide if the target object is pinned,
 * because the store might have happened but the remset not yet added,
 * but in that case the target must be pinned.  We might theoretically
 * miss some missing remsets this way, but it's very unlikely.
 */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
	if (*(ptr) && mono_sgen_ptr_in_nursery ((char*)*(ptr))) { \
		if (!mono_sgen_ssb_find_address ((char*)(ptr)) && (!use_cardtable || !sgen_card_table_address_is_marked ((mword)ptr))) { \
			fprintf (gc_debug_file, "Oldspace->newspace reference %p at offset %td in object %p (%s.%s) not found in remsets.\n", *(ptr), (char*)(ptr) - (char*)(obj), (obj), ((MonoObject*)(obj))->vtable->klass->name_space, ((MonoObject*)(obj))->vtable->klass->name); \
			binary_protocol_missing_remset ((obj), (gpointer)LOAD_VTABLE ((obj)), (char*)(ptr) - (char*)(obj), *(ptr), (gpointer)LOAD_VTABLE(*(ptr)), object_is_pinned (*(ptr))); \
			if (!object_is_pinned (*(ptr)))								\
				missing_remsets = TRUE;									\
		}																\
	}																	\
	} while (0)

/*
 * Check that each object reference which points into the nursery can
 * be found in the remembered sets.
 */
static void
check_consistency_callback (char *start, size_t size, void *dummy)
{
	GCVTable *vt = (GCVTable*)LOAD_VTABLE (start);
	DEBUG (8, fprintf (gc_debug_file, "Scanning object %p, vtable: %p (%s)\n", start, vt, vt->klass->name));

#define SCAN_OBJECT_ACTION
#include "sgen-scan-object.h"
}

/*
 * Perform consistency check of the heap.
 *
 * Assumes the world is stopped.
 */
void
mono_sgen_check_consistency (void)
{
	// Need to add more checks

	missing_remsets = FALSE;

	DEBUG (1, fprintf (gc_debug_file, "Begin heap consistency check...\n"));

	// Check that oldspace->newspace pointers are registered with the collector
	major_collector.iterate_objects (TRUE, TRUE, (IterateObjectCallbackFunc)check_consistency_callback, NULL);

	mono_sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_consistency_callback, NULL);

	DEBUG (1, fprintf (gc_debug_file, "Heap consistency check done.\n"));

	if (!binary_protocol_is_enabled ())
		g_assert (!missing_remsets);
}


#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {					\
		if (*(ptr) && !LOAD_VTABLE (*(ptr)))						\
			g_error ("Could not load vtable for obj %p slot %d (size %d)", obj, (char*)ptr - (char*)obj, safe_object_get_size ((MonoObject*)obj));		\
	} while (0)

static void
check_major_refs_callback (char *start, size_t size, void *dummy)
{
#define SCAN_OBJECT_ACTION
#include "sgen-scan-object.h"
}

void
mono_sgen_check_major_refs (void)
{
	major_collector.iterate_objects (TRUE, TRUE, (IterateObjectCallbackFunc)check_major_refs_callback, NULL);
	mono_sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_major_refs_callback, NULL);
}

/* Check that the reference is valid */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr)) {	\
			g_assert (mono_sgen_safe_name (*(ptr)) != NULL);	\
		}	\
	} while (0)

/*
 * check_object:
 *
 *   Perform consistency check on an object. Currently we only check that the
 * reference fields are valid.
 */
void
check_object (char *start)
{
	if (!start)
		return;

#include "sgen-scan-object.h"
}

#endif /*HAVE_SGEN_GC*/
