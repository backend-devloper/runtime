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

#ifdef SGEN_PARALLEL_MARK

typedef struct _WorkerData WorkerData;
struct _WorkerData {
	pthread_t thread;
	MonoSemType start_worker_sem;
	gboolean is_working;
	GrayQueue private_gray_queue; /* only read/written by worker thread */
	int shared_buffer_increment;
	int shared_buffer_index;
};

static int workers_num;
static WorkerData *workers_data;
static WorkerData workers_gc_thread_data;

static int workers_num_working;

static GrayQueue workers_distribute_gray_queue;
/* must be a power of 2 */
#define WORKERS_SHARED_BUFFER_SIZE	16
static GrayQueueSection *workers_shared_buffer [WORKERS_SHARED_BUFFER_SIZE];
static int workers_shared_buffer_used;

static const int workers_primes [] = { 3, 5, 7, 11, 13, 17, 23, 29 };

static MonoSemType workers_done_sem;

static void
workers_gray_queue_share_redirect (GrayQueue *queue)
{
	GrayQueueSection *section;
	WorkerData *data = queue->alloc_prepare_data;
	int increment = data->shared_buffer_increment;

	while ((section = gray_object_dequeue_section (queue))) {
		int i, index;

		if (workers_shared_buffer_used == WORKERS_SHARED_BUFFER_SIZE) {
			gray_object_enqueue_section (queue, section);
			return;
		}

		index = data->shared_buffer_index;
		for (i = 0; i < WORKERS_SHARED_BUFFER_SIZE; ++i) {
			GrayQueueSection *old = workers_shared_buffer [index];
			if (!old) {
				if (SGEN_CAS_PTR ((void**)&workers_shared_buffer [index], section, NULL) == NULL) {
					SGEN_ATOMIC_ADD (workers_shared_buffer_used, 1);
					//g_print ("thread %d put section %d\n", data - workers_data, index);
					break;
				}
			}
			index = (index + increment) & (WORKERS_SHARED_BUFFER_SIZE - 1);
		}
		data->shared_buffer_index = index;

		if (i == WORKERS_SHARED_BUFFER_SIZE) {
			/* unsuccessful */
			gray_object_enqueue_section (queue, section);
			return;
		}
	}
}

static gboolean
workers_get_work (WorkerData *data)
{
	int i, index;
	int increment = data->shared_buffer_increment;

	index = data->shared_buffer_index;
	for (i = 0; i < WORKERS_SHARED_BUFFER_SIZE; ++i) {
		GrayQueueSection *section;

		do {
			section = workers_shared_buffer [index];
			if (!section)
				break;
		} while (SGEN_CAS_PTR ((void**)&workers_shared_buffer [index], NULL, section) != section);

		if (section) {
			SGEN_ATOMIC_ADD (workers_shared_buffer_used, -1);
			gray_object_enqueue_section (&data->private_gray_queue, section);
			data->shared_buffer_index = index;
			//g_print ("thread %d popped section %d\n", data - workers_data, index);
			return TRUE;
		}

		index = (index + increment) & (WORKERS_SHARED_BUFFER_SIZE - 1);
	}

	data->shared_buffer_index = index;
	return FALSE;
}

/* returns the new value */
static int
workers_change_num_working (int delta)
{
	int old, new;
	do {
		old = workers_num_working;
		new = old + delta;
	} while (InterlockedCompareExchange (&workers_num_working, new, old) != old);
	return new;
}

static void*
workers_thread_func (void *data_untyped)
{
	WorkerData *data = data_untyped;

	for (;;) {
		//g_print ("worker waiting for start %d\n", data->start_worker_sem);

		MONO_SEM_WAIT (&data->start_worker_sem);

		//g_print ("worker starting\n");

		for (;;) {
			do {
				drain_gray_stack (&data->private_gray_queue);
			} while (workers_get_work (data));

			/*
			 * FIXME: This might never terminate with
			 * multiple threads!
			 */

			if (workers_change_num_working (-1) == 0)
				break;

			/* we weren't the last one working */
			//g_print ("sleeping\n");
			usleep (5000);
			workers_change_num_working (1);
		}

		gray_object_queue_init (&data->private_gray_queue);

		MONO_SEM_POST (&workers_done_sem);

		//g_print ("worker done\n");
	}
}

static void
workers_distribute_gray_queue_sections (void)
{
	workers_gray_queue_share_redirect (&workers_distribute_gray_queue);
}

static void
workers_init (int num_workers)
{
	int i;

	//g_print ("initing %d workers\n", num_workers);

	workers_num = num_workers;
	workers_data = mono_sgen_alloc_internal_dynamic (sizeof (WorkerData) * num_workers, INTERNAL_MEM_WORKER_DATA);
	MONO_SEM_INIT (&workers_done_sem, 0);
	workers_gc_thread_data.shared_buffer_increment = 1;
	workers_gc_thread_data.shared_buffer_index = 0;
	gray_object_queue_init_with_alloc_prepare (&workers_distribute_gray_queue,
			workers_gray_queue_share_redirect, &workers_gc_thread_data);

	g_assert (num_workers <= sizeof (workers_primes) / sizeof (workers_primes [0]));
	for (i = 0; i < workers_num; ++i) {
		workers_data [i].shared_buffer_increment = workers_primes [i];
		workers_data [i].shared_buffer_index = 0;
		gray_object_queue_init_with_alloc_prepare (&workers_data [i].private_gray_queue,
				workers_gray_queue_share_redirect,  &workers_data [i]);
	}
}

/* only the GC thread is allowed to start and join workers */

static void
workers_start_worker (int index)
{
	g_assert (index >= 0 && index < workers_num);

	if (workers_data [index].is_working)
		return;

	if (!workers_data [index].thread) {
		//g_print ("initing thread %d\n", index);
		MONO_SEM_INIT (&workers_data [index].start_worker_sem, 0);
		pthread_create (&workers_data [index].thread, NULL, workers_thread_func, &workers_data [index]);
	}

	workers_data [index].is_working = TRUE;
	MONO_SEM_POST (&workers_data [index].start_worker_sem);
	//g_print ("posted thread start %d %d\n", index, workers_data [index].start_worker_sem);
}

static void
workers_start_all_workers (int num_additional_workers)
{
	int i;

	g_assert (workers_num_working == 0);
	workers_num_working = workers_num + num_additional_workers;

	for (i = 0; i < workers_num; ++i)
		workers_start_worker (i);
}

static void
workers_join (void)
{
	int i;

	//g_print ("joining\n");
	for (i = 0; i < workers_num; ++i) {
		if (workers_data [i].is_working)
			MONO_SEM_WAIT (&workers_done_sem);
	}
	for (i = 0; i < workers_num; ++i)
		workers_data [i].is_working = FALSE;
	//g_print ("joined\n");

	g_assert (workers_num_working == 0);
	g_assert (workers_shared_buffer_used == 0);

	for (i = 0; i < WORKERS_SHARED_BUFFER_SIZE; ++i)
		g_assert (!workers_shared_buffer [i]);
}

gboolean
mono_sgen_is_worker_thread (pthread_t thread)
{
	int i;

	for (i = 0; i < workers_num; ++i) {
		if (workers_data [i].thread == thread)
			return TRUE;
	}
	return FALSE;
}

#else

#define workers_distribute_gray_queue gray_queue

static int
workers_change_num_working (int delta)
{
	return -1;
}

static void
workers_distribute_gray_queue_sections (void)
{
}

static void
workers_init (int num_workers)
{
}

static void
workers_start_all_workers (int num_additional_workers)
{
}

static void
workers_join (void)
{
}

gboolean
mono_sgen_is_worker_thread (pthread_t thread)
{
	return FALSE;
}

#endif
