/*
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
extern long long stat_scan_object_called_nursery;

#if defined(SGEN_SIMPLE_NURSERY)
#define SERIAL_SCAN_OBJECT simple_nursery_serial_scan_object
#define SERIAL_SCAN_VTYPE simple_nursery_serial_scan_vtype
#define PARALLEL_SCAN_OBJECT simple_nursery_parallel_scan_object
#define PARALLEL_SCAN_VTYPE simple_nursery_parallel_scan_vtype

#elif defined (SGEN_SPLIT_NURSERY)
#define SERIAL_SCAN_OBJECT split_nursery_serial_scan_object
#define SERIAL_SCAN_VTYPE split_nursery_serial_scan_vtype
#define PARALLEL_SCAN_OBJECT split_nursery_parallel_scan_object
#define PARALLEL_SCAN_VTYPE split_nursery_parallel_scan_vtype

#else
#error "Please define GC_CONF_NAME"
#endif

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		void *__old = *(ptr);	\
		void *__copy;		\
		if (__old) {	\
			PARALLEL_COPY_OBJECT ((ptr), queue);	\
			__copy = *(ptr);	\
			SGEN_COND_LOG (9, __old != __copy, "Overwrote field at %p with %p (was: %p)", (ptr), *(ptr), __old);	\
			if (G_UNLIKELY (sgen_ptr_in_nursery (__copy) && !sgen_ptr_in_nursery ((ptr)))) \
				sgen_add_to_global_remset ((ptr));	\
		}	\
	} while (0)

/*
 * Scan the object pointed to by @start for references to
 * other objects between @from_start and @from_end and copy
 * them to the gray_objects area.
 */
static void
PARALLEL_SCAN_OBJECT (char *start, SgenGrayQueue *queue)
{
#include "sgen-scan-object.h"

	HEAVY_STAT (++stat_scan_object_called_nursery);
}

/*
 * scan_vtype:
 *
 * Scan the valuetype pointed to by START, described by DESC for references to
 * other objects between @from_start and @from_end and copy them to the gray_objects area.
 * Returns a pointer to the end of the object.
 */
static void
PARALLEL_SCAN_VTYPE (char *start, mword desc, SgenGrayQueue *queue)
{
	/* The descriptors include info about the MonoObject header as well */
	start -= sizeof (MonoObject);

#define SCAN_OBJECT_NOVTABLE
#include "sgen-scan-object.h"
}

#undef HANDLE_PTR
/* Global remsets are handled in SERIAL_COPY_OBJECT_FROM_OBJ */
#define HANDLE_PTR(ptr,obj)	do {	\
		void *__old = *(ptr);	\
		void *__copy;		\
		if (__old) {	\
			SERIAL_COPY_OBJECT ((ptr), queue);	\
			__copy = *(ptr);	\
			if (G_UNLIKELY (sgen_ptr_in_nursery (__copy) && !sgen_ptr_in_nursery ((ptr)))) \
				sgen_add_to_global_remset ((ptr));	\
			SGEN_COND_LOG (9, __old != *(ptr), "Overwrote field at %p with %p (was: %p)", (ptr), *(ptr), __old); \
		}	\
	} while (0)

static void
SERIAL_SCAN_OBJECT (char *start, SgenGrayQueue *queue)
{
#include "sgen-scan-object.h"

	HEAVY_STAT (++stat_scan_object_called_nursery);
}

static void
SERIAL_SCAN_VTYPE (char *start, mword desc, SgenGrayQueue *queue)
{
	/* The descriptors include info about the MonoObject header as well */
	start -= sizeof (MonoObject);

#define SCAN_OBJECT_NOVTABLE
#include "sgen-scan-object.h"
}

#define FILL_MINOR_COLLECTOR_SCAN_OBJECT(collector)	do {			\
		(collector)->parallel_ops.scan_object = PARALLEL_SCAN_OBJECT;	\
		(collector)->parallel_ops.scan_vtype = PARALLEL_SCAN_VTYPE;	\
		(collector)->serial_ops.scan_object = SERIAL_SCAN_OBJECT;	\
		(collector)->serial_ops.scan_vtype = SERIAL_SCAN_VTYPE; \
	} while (0)
