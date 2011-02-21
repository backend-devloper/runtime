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

#define STEALABLE_STACK_SIZE	512

typedef struct _WorkerData WorkerData;
struct _WorkerData {
	pthread_t thread;
	void *major_collector_data;

	GrayQueue private_gray_queue; /* only read/written by worker thread */

	pthread_mutex_t stealable_stack_mutex;
	volatile int stealable_stack_fill;
	char *stealable_stack [STEALABLE_STACK_SIZE];
};

static int workers_num;
static WorkerData *workers_data;
static WorkerData workers_gc_thread_data;

static GrayQueue workers_distribute_gray_queue;

#define WORKERS_DISTRIBUTE_GRAY_QUEUE (major_collector.is_parallel ? &workers_distribute_gray_queue : &gray_queue)

static volatile gboolean workers_gc_in_progress = FALSE;
static gboolean workers_started = FALSE;
static volatile int workers_num_waiting = 0;
static MonoSemType workers_waiting_sem;
static MonoSemType workers_done_sem;
static volatile int workers_done_posted = 0;

static long long stat_workers_stolen_from_self;
static long long stat_workers_stolen_from_others;
static long long stat_workers_num_waited;

static void
workers_wake_up_all (void)
{
	int max;
	int i;

	max = workers_num_waiting;
	for (i = 0; i < max; ++i) {
		int num;
		do {
			num = workers_num_waiting;
			if (num == 0)
				return;
		} while (InterlockedCompareExchange (&workers_num_waiting, num - 1, num) != num);
		MONO_SEM_POST (&workers_waiting_sem);
	}
}

static void
workers_wait (void)
{
	int num;
	++stat_workers_num_waited;
	do {
		num = workers_num_waiting;
	} while (InterlockedCompareExchange (&workers_num_waiting, num + 1, num) != num);
	if (num + 1 == workers_num && !workers_gc_in_progress) {
		/* Make sure the done semaphore is only posted once. */
		int posted;
		do {
			posted = workers_done_posted;
			if (posted)
				break;
		} while (InterlockedCompareExchange (&workers_done_posted, 1, 0) != 0);
		if (!posted)
			MONO_SEM_POST (&workers_done_sem);
	}
	MONO_SEM_WAIT (&workers_waiting_sem);
}

static void
workers_gray_queue_share_redirect (GrayQueue *queue)
{
	GrayQueueSection *section;
	WorkerData *data = queue->alloc_prepare_data;

	if (data->stealable_stack_fill) {
		/*
		 * There are still objects in the stealable stack, so
		 * wake up any workers that might be sleeping
		 */
		if (workers_gc_in_progress)
			workers_wake_up_all ();
		return;
	}

	/* The stealable stack is empty, so fill it. */
	pthread_mutex_lock (&data->stealable_stack_mutex);

	while (data->stealable_stack_fill < STEALABLE_STACK_SIZE &&
			(section = gray_object_dequeue_section (queue))) {
		int num = MIN (section->end, STEALABLE_STACK_SIZE - data->stealable_stack_fill);

		memcpy (data->stealable_stack + data->stealable_stack_fill,
				section->objects + section->end - num,
				sizeof (char*) * num);

		section->end -= num;
		data->stealable_stack_fill += num;

		if (section->end)
			gray_object_enqueue_section (queue, section);
		else
			gray_object_free_queue_section (section, queue->allocator);
	}

	pthread_mutex_unlock (&data->stealable_stack_mutex);

	if (workers_gc_in_progress)
		workers_wake_up_all ();
}

static gboolean
workers_steal (WorkerData *data, WorkerData *victim_data)
{
	GrayQueue *queue = &data->private_gray_queue;
	int num, n;

	g_assert (!queue->first);

	if (!victim_data->stealable_stack_fill)
		return FALSE;

	if (pthread_mutex_trylock (&victim_data->stealable_stack_mutex))
		return FALSE;

	n = num = (victim_data->stealable_stack_fill + 1) / 2;
	/* We're stealing num entries. */

	while (n > 0) {
		int m = MIN (SGEN_GRAY_QUEUE_SECTION_SIZE, n);
		n -= m;

		gray_object_alloc_queue_section (queue);
		memcpy (queue->first->objects,
				victim_data->stealable_stack + victim_data->stealable_stack_fill - num + n,
				sizeof (char*) * m);
		queue->first->end = m;
	}

	victim_data->stealable_stack_fill -= num;

	pthread_mutex_unlock (&victim_data->stealable_stack_mutex);

	if (data == victim_data)
		stat_workers_stolen_from_self += num;
	else
		stat_workers_stolen_from_others += num;

	return num != 0;
}

static gboolean
workers_get_work (WorkerData *data)
{
	g_assert (gray_object_queue_is_empty (&data->private_gray_queue));

	for (;;) {
		int i;

		/* Try to steal from our own stack. */
		if (workers_steal (data, data))
			return TRUE;

		/* Then from the GC thread's stack. */
		if (workers_steal (data, &workers_gc_thread_data))
			return TRUE;

		/* Finally, from another worker. */
		for (i = 0; i < workers_num; ++i) {
			WorkerData *victim_data = &workers_data [i];
			if (data == victim_data)
				continue;
			if (workers_steal (data, victim_data))
				return TRUE;
		}

		/* Nobody to steal from, so wait. */
		g_assert (gray_object_queue_is_empty (&data->private_gray_queue));
		workers_wait ();
	}
}

static void*
workers_thread_func (void *data_untyped)
{
	WorkerData *data = data_untyped;
	SgenInternalAllocator allocator;

	if (major_collector.init_worker_thread)
		major_collector.init_worker_thread (data->major_collector_data);

	memset (&allocator, 0, sizeof (allocator));

	gray_object_queue_init_with_alloc_prepare (&data->private_gray_queue, &allocator,
			workers_gray_queue_share_redirect, data);

	for (;;) {
		gboolean got_work = workers_get_work (data);
		g_assert (got_work);
		g_assert (!gray_object_queue_is_empty (&data->private_gray_queue));

		drain_gray_stack (&data->private_gray_queue);
		g_assert (gray_object_queue_is_empty (&data->private_gray_queue));

		gray_object_queue_init (&data->private_gray_queue, &allocator);
	}

	/* dummy return to make compilers happy */
	return NULL;
}

static void
workers_distribute_gray_queue_sections (void)
{
	if (!major_collector.is_parallel)
		return;

	workers_gray_queue_share_redirect (&workers_distribute_gray_queue);
}

static void
workers_init (int num_workers)
{
	int i;

	if (!major_collector.is_parallel)
		return;

	//g_print ("initing %d workers\n", num_workers);

	workers_num = num_workers;

	workers_data = mono_sgen_alloc_internal_dynamic (sizeof (WorkerData) * num_workers, INTERNAL_MEM_WORKER_DATA);
	memset (workers_data, 0, sizeof (WorkerData) * num_workers);

	MONO_SEM_INIT (&workers_waiting_sem, 0);
	MONO_SEM_INIT (&workers_done_sem, 0);

	gray_object_queue_init_with_alloc_prepare (&workers_distribute_gray_queue, mono_sgen_get_unmanaged_allocator (),
			workers_gray_queue_share_redirect, &workers_gc_thread_data);
	pthread_mutex_init (&workers_gc_thread_data.stealable_stack_mutex, NULL);
	workers_gc_thread_data.stealable_stack_fill = 0;

	if (major_collector.alloc_worker_data)
		workers_gc_thread_data.major_collector_data = major_collector.alloc_worker_data ();

	for (i = 0; i < workers_num; ++i) {
		/* private gray queue is inited by the thread itself */
		pthread_mutex_init (&workers_data [i].stealable_stack_mutex, NULL);
		workers_data [i].stealable_stack_fill = 0;

		if (major_collector.alloc_worker_data)
			workers_data [i].major_collector_data = major_collector.alloc_worker_data ();
	}

	mono_counters_register ("Stolen from self", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_workers_stolen_from_self);
	mono_counters_register ("Stolen from others", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_workers_stolen_from_others);
	mono_counters_register ("# workers waited", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_workers_num_waited);
}

/* only the GC thread is allowed to start and join workers */

static void
workers_start_worker (int index)
{
	g_assert (index >= 0 && index < workers_num);

	g_assert (!workers_data [index].thread);
	pthread_create (&workers_data [index].thread, NULL, workers_thread_func, &workers_data [index]);
}

static void
workers_start_all_workers (void)
{
	int i;

	if (!major_collector.is_parallel)
		return;

	if (major_collector.init_worker_thread)
		major_collector.init_worker_thread (workers_gc_thread_data.major_collector_data);

	g_assert (!workers_gc_in_progress);
	workers_gc_in_progress = TRUE;
	workers_done_posted = 0;

	if (workers_started) {
		g_assert (workers_num_waiting == workers_num);
		workers_wake_up_all ();
		return;
	}

	for (i = 0; i < workers_num; ++i)
		workers_start_worker (i);

	workers_started = TRUE;
}

static void
workers_join (void)
{
	int i;

	if (!major_collector.is_parallel)
		return;

	g_assert (gray_object_queue_is_empty (&workers_gc_thread_data.private_gray_queue));
	g_assert (gray_object_queue_is_empty (&workers_distribute_gray_queue));

	g_assert (workers_gc_in_progress);
	workers_gc_in_progress = FALSE;
	if (workers_num_waiting == workers_num)
		workers_wake_up_all ();
	MONO_SEM_WAIT (&workers_done_sem);

	if (major_collector.reset_worker_data) {
		for (i = 0; i < workers_num; ++i)
			major_collector.reset_worker_data (workers_data [i].major_collector_data);
	}

	g_assert (workers_done_posted);
	g_assert (workers_num_waiting == workers_num);

	g_assert (!workers_gc_thread_data.stealable_stack_fill);
	g_assert (gray_object_queue_is_empty (&workers_gc_thread_data.private_gray_queue));
	for (i = 0; i < workers_num; ++i) {
		g_assert (!workers_data [i].stealable_stack_fill);
		g_assert (gray_object_queue_is_empty (&workers_data [i].private_gray_queue));
	}
}

gboolean
mono_sgen_is_worker_thread (pthread_t thread)
{
	int i;

	if (major_collector.is_worker_thread && major_collector.is_worker_thread (thread))
		return TRUE;

	if (!major_collector.is_parallel)
		return FALSE;

	for (i = 0; i < workers_num; ++i) {
		if (workers_data [i].thread == thread)
			return TRUE;
	}
	return FALSE;
}
