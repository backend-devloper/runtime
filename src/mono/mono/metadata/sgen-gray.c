/*
 * sgen-gray.c: Gray queue management.
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright (C) 2012 Xamarin Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License 2.0 as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License 2.0 along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "config.h"
#ifdef HAVE_SGEN_GC

#include "metadata/sgen-gc.h"
#include "utils/mono-counters.h"

#define GRAY_QUEUE_LENGTH_LIMIT	64

static void
lock_queue (SgenGrayQueue *queue)
{
	if (!queue->locked)
		return;

	mono_mutex_lock (&queue->lock);
}

static void
unlock_queue (SgenGrayQueue *queue)
{
	if (!queue->locked)
		return;

	mono_mutex_unlock (&queue->lock);
}

void
sgen_gray_object_alloc_queue_section (SgenGrayQueue *queue)
{
	GrayQueueSection *section;

	lock_queue (queue);

	if (queue->alloc_prepare_func)
		queue->alloc_prepare_func (queue);

	if (queue->free_list) {
		/* Use the previously allocated queue sections if possible */
		section = queue->free_list;
		queue->free_list = section->next;
	} else {
		/* Allocate a new section */
		section = sgen_alloc_internal (INTERNAL_MEM_GRAY_QUEUE);
	}

	section->end = 0;

	/* Link it with the others */
	section->next = queue->first;
	queue->first = section;

	unlock_queue (queue);
}

void
sgen_gray_object_free_queue_section (GrayQueueSection *section)
{
	sgen_free_internal (section, INTERNAL_MEM_GRAY_QUEUE);
}

/*
 * The following two functions are called in the inner loops of the
 * collector, so they need to be as fast as possible.  We have macros
 * for them in sgen-gc.h.
 */

void
sgen_gray_object_enqueue (SgenGrayQueue *queue, char *obj)
{
	SGEN_ASSERT (9, obj, "enqueueing a null object");
	//sgen_check_objref (obj);

	lock_queue (queue);

#ifdef SGEN_CHECK_GRAY_OBJECT_ENQUEUE
	if (queue->enqueue_check_func)
		queue->enqueue_check_func (queue, obj);
#endif

	if (G_UNLIKELY (!queue->first || queue->first->end == SGEN_GRAY_QUEUE_SECTION_SIZE))
		sgen_gray_object_alloc_queue_section (queue);
	SGEN_ASSERT (9, queue->first->end < SGEN_GRAY_QUEUE_SECTION_SIZE, "gray queue %p overflow, first %p, end %d", queue, queue->first, queue->first->end);
	queue->first->objects [queue->first->end++] = obj;

	unlock_queue (queue);
}

char*
sgen_gray_object_dequeue (SgenGrayQueue *queue)
{
	char *obj;

	if (sgen_gray_object_queue_is_empty (queue))
		return NULL;

	if (queue->locked) {
		lock_queue (queue);
		if (sgen_gray_object_queue_is_empty (queue)) {
			unlock_queue (queue);
			return NULL;
		}
	}

	SGEN_ASSERT (9, queue->first->end, "gray queue %p underflow, first %p, end %d", queue, queue->first, queue->first->end);

	obj = queue->first->objects [--queue->first->end];

	if (G_UNLIKELY (queue->first->end == 0)) {
		GrayQueueSection *section = queue->first;
		queue->first = section->next;
		section->next = queue->free_list;
		queue->free_list = section;
	}

	unlock_queue (queue);

	return obj;
}

GrayQueueSection*
sgen_gray_object_dequeue_section (SgenGrayQueue *queue)
{
	GrayQueueSection *section;

	if (!queue->first)
		return NULL;

	if (queue->locked) {
		lock_queue (queue);
		if (!queue->first) {
			unlock_queue (queue);
			return NULL;
		}
	}

	section = queue->first;
	queue->first = section->next;

	section->next = NULL;

	unlock_queue (queue);

	return section;
}

void
sgen_gray_object_enqueue_section (SgenGrayQueue *queue, GrayQueueSection *section)
{
	lock_queue (queue);

	section->next = queue->first;
	queue->first = section;
#ifdef SGEN_CHECK_GRAY_OBJECT_ENQUEUE
	if (queue->enqueue_check_func) {
		int i;
		for (i = 0; i < section->end; ++i)
			queue->enqueue_check_func (queue, section->objects [i]);
	}
#endif

	unlock_queue (queue);
}

void
sgen_gray_object_queue_init (SgenGrayQueue *queue, GrayQueueEnqueueCheckFunc enqueue_check_func, gboolean locked)
{
	GrayQueueSection *section, *next;
	int i;

	g_assert (sgen_gray_object_queue_is_empty (queue));

	queue->alloc_prepare_func = NULL;
	queue->alloc_prepare_data = NULL;
#ifdef SGEN_CHECK_GRAY_OBJECT_ENQUEUE
	queue->enqueue_check_func = enqueue_check_func;
#endif

	queue->locked = locked;
	if (locked) {
		mono_mutexattr_t attr;
		mono_mutexattr_init (&attr);
		mono_mutexattr_settype (&attr, MONO_MUTEX_RECURSIVE);
		mono_mutex_init (&queue->lock, &attr);
	}

	/* Free the extra sections allocated during the last collection */
	i = 0;
	for (section = queue->free_list; section && i < GRAY_QUEUE_LENGTH_LIMIT - 1; section = section->next)
		i ++;
	if (!section)
		return;
	while (section->next) {
		next = section->next;
		section->next = next->next;
		sgen_gray_object_free_queue_section (next);
	}
}

static void
invalid_prepare_func (SgenGrayQueue *queue)
{
	g_assert_not_reached ();
}

void
sgen_gray_object_queue_init_invalid (SgenGrayQueue *queue)
{
	sgen_gray_object_queue_init (queue, NULL, FALSE);
	queue->alloc_prepare_func = invalid_prepare_func;
	queue->alloc_prepare_data = NULL;
}

void
sgen_gray_object_queue_init_with_alloc_prepare (SgenGrayQueue *queue, GrayQueueEnqueueCheckFunc enqueue_check_func,
		gboolean locked,
		GrayQueueAllocPrepareFunc alloc_prepare_func, void *data)
{
	sgen_gray_object_queue_init (queue, enqueue_check_func, locked);
	queue->alloc_prepare_func = alloc_prepare_func;
	queue->alloc_prepare_data = data;
}

#endif
