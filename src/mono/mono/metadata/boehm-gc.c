/*
 * boehm-gc.c: GC implementation using either the installed or included Boehm GC.
 *
 */

#include "config.h"
#define GC_I_HIDE_POINTERS
#include <mono/os/gc_wrapper.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/profiler-private.h>

#if HAVE_BOEHM_GC

void
mono_gc_collect (int generation)
{
	GC_gcollect ();
}

int
mono_gc_max_generation (void)
{
	return 0;
}

gint64
mono_gc_get_used_size (void)
{
	return GC_get_heap_size () - GC_get_free_bytes ();
}

gint64
mono_gc_get_heap_size (void)
{
	return GC_get_heap_size ();
}

void
mono_gc_disable (void)
{
#ifdef HAVE_GC_ENABLE
	GC_disable ();
#else
	g_assert_not_reached ();
#endif
}

void
mono_gc_enable (void)
{
#ifdef HAVE_GC_ENABLE
	GC_enable ();
#else
	g_assert_not_reached ();
#endif
}

gboolean
mono_gc_is_gc_thread (void)
{
#ifdef USE_INCLUDED_LIBGC
	return GC_thread_is_registered ();
#else
	return TRUE;
#endif
}

gboolean
mono_gc_register_thread (void *baseptr)
{
	if (mono_gc_is_gc_thread())
		return TRUE;
#if defined(USE_INCLUDED_LIBGC) && !defined(PLATFORM_WIN32)
	return GC_thread_register_foreign (baseptr);
#else
	return FALSE;
#endif
}

extern int GC_is_marked (void *p);

gboolean
mono_object_is_alive (MonoObject* o)
{
#ifdef USE_INCLUDED_LIBGC
	return GC_is_marked (o);
#else
	return TRUE;
#endif
}

#ifdef USE_INCLUDED_LIBGC

static void
on_gc_notification (GCEventType event)
{
	mono_profiler_gc_event ((MonoGCEvent) event, 0);
}
 
static void
on_gc_heap_resize (size_t new_size)
{
	mono_profiler_gc_heap_resize (new_size);
}

void
mono_gc_enable_events (void)
{
	GC_notify_event = on_gc_notification;
	GC_on_heap_resize = on_gc_heap_resize;
}

#else

void
mono_gc_enable_events (void)
{
}

#endif

void
mono_gc_weak_link_add (void **link_addr, MonoObject *obj)
{
	/* libgc requires that we use HIDE_POINTER... */
	*link_addr = (void*)HIDE_POINTER (obj);
	GC_GENERAL_REGISTER_DISAPPEARING_LINK (link_addr, obj);
}

void
mono_gc_weak_link_remove (void **link_addr)
{
	GC_unregister_disappearing_link (link_addr);
}

MonoObject*
mono_gc_weak_link_get (void **link_addr)
{
	MonoObject *obj = REVEAL_POINTER (*link_addr);
	if (obj == (MonoObject *) -1)
		return NULL;
	return obj;
}

void*
mono_gc_make_descr_from_bitmap (unsigned int *bitmap, int numbits)
{
	return NULL;
}

void*
mono_gc_alloc_fixed (size_t size, void *descr)
{
	return GC_MALLOC (size);
}

void
mono_gc_free_fixed (void* addr)
{
}

#endif /* no Boehm GC */

