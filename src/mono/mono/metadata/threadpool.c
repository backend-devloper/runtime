/*
 * threadpool.c: global thread pool
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/exception.h>
#include <mono/io-layer/io-layer.h>
#include <mono/os/gc_wrapper.h>

#include "threadpool.h"

/* FIXME:
 * - worker threads need to be initialized correctly.
 * - worker threads should be domain specific
 */

/* maximum number of worker threads */
int mono_worker_threads = 1;

static int workers = 0;

typedef struct {
	MonoMethodMessage *msg;
	HANDLE             wait_semaphore;
	MonoMethod        *cb_method;
	MonoDelegate      *cb_target;
	MonoObject        *state;
	MonoObject        *res;
	MonoArray         *out_args;
} ASyncCall;

static void async_invoke_thread (void);

static GList *async_call_queue = NULL;

static void
mono_async_invoke (MonoAsyncResult *ares)
{
	ASyncCall *ac = (ASyncCall *)ares->data;

	ac->msg->exc = NULL;
	ac->res = mono_message_invoke (ares->async_delegate, ac->msg, 
				       &ac->msg->exc, &ac->out_args);

	ares->completed = 1;
		
	/* notify listeners */
	ReleaseSemaphore (ac->wait_semaphore, 0x7fffffff, NULL);

	/* call async callback if cb_method != null*/
	if (ac->cb_method) {
		MonoObject *exc = NULL;
		void *pa = &ares;
		mono_runtime_invoke (ac->cb_method, ac->cb_target, pa, &exc);
		if (!ac->msg->exc)
			ac->msg->exc = exc;
	}
}

MonoAsyncResult *
mono_thread_pool_add (MonoObject *target, MonoMethodMessage *msg, MonoDelegate *async_callback,
		      MonoObject *state)
{
	MonoDomain *domain = mono_domain_get ();
	MonoAsyncResult *ares;
	ASyncCall *ac;

#ifdef HAVE_BOEHM_GC
	ac = GC_MALLOC (sizeof (ASyncCall));
#else
	/* We'll leak the semaphore... */
	ac = g_new0 (ASyncCall, 1);
#endif
	ac->wait_semaphore = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);	
	ac->msg = msg;
	ac->state = state;

	if (async_callback) {
		ac->cb_method = mono_get_delegate_invoke (((MonoObject *)async_callback)->vtable->klass);
		ac->cb_target = async_callback;
	}

	ares = mono_async_result_new (domain, ac->wait_semaphore, ac->state, ac);
	ares->async_delegate = target;

	EnterCriticalSection (&mono_delegate_section);	
	async_call_queue = g_list_append (async_call_queue, ares); 
	ReleaseSemaphore (mono_delegate_semaphore, 1, NULL);

	if (workers == 0) {
		workers++;
		mono_thread_create (domain, async_invoke_thread, NULL);
	}
	LeaveCriticalSection (&mono_delegate_section);

	return ares;
}

MonoObject *
mono_thread_pool_finish (MonoAsyncResult *ares, MonoArray **out_args, MonoObject **exc)
{
	ASyncCall *ac;
	GList *l;

	*exc = NULL;
	*out_args = NULL;

	EnterCriticalSection (&mono_delegate_section);	
	/* check if already finished */
	if (ares->endinvoke_called) {
		*exc = (MonoObject *)mono_exception_from_name (mono_defaults.corlib, "System", 
					      "InvalidOperationException");
		LeaveCriticalSection (&mono_delegate_section);
		return NULL;
	}

	ares->endinvoke_called = 1;
	ac = (ASyncCall *)ares->data;

	g_assert (ac != NULL);

	if ((l = g_list_find (async_call_queue, ares))) {
		async_call_queue = g_list_remove_link (async_call_queue, l);
		mono_async_invoke (ares);
	}		
	LeaveCriticalSection (&mono_delegate_section);
	
	/* wait until we are really finished */
	WaitForSingleObject (ac->wait_semaphore, INFINITE);

	*exc = ac->msg->exc;
	*out_args = ac->out_args;

	return ac->res;
}

static void
async_invoke_thread ()
{
	MonoDomain *domain;
 
	for (;;) {
		MonoAsyncResult *ar;
		gboolean new_worker = FALSE;

		if (WaitForSingleObject (mono_delegate_semaphore, 500) == WAIT_TIMEOUT) {
			EnterCriticalSection (&mono_delegate_section);
			workers--;
			LeaveCriticalSection (&mono_delegate_section);
			ExitThread (0);
		}
		
		ar = NULL;
		EnterCriticalSection (&mono_delegate_section);
		
		if (async_call_queue) {
			if ((g_list_length (async_call_queue) > 1) && 
			    (workers < mono_worker_threads)) {
				new_worker = TRUE;
				workers++;
			}

			ar = (MonoAsyncResult *)async_call_queue->data;
			async_call_queue = g_list_remove_link (async_call_queue, async_call_queue); 

		}

		LeaveCriticalSection (&mono_delegate_section);

		if (!ar)
			continue;
		
		/* worker threads invokes methods in different domains,
		 * so we need to set the right domain here */
		domain = ((MonoObject *)ar)->vtable->domain;
		mono_domain_set (domain);

		if (new_worker)
			mono_thread_create (domain, async_invoke_thread, NULL);

		mono_async_invoke (ar);
	}

	g_assert_not_reached ();
}
