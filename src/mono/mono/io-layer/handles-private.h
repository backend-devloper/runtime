/*
 * handles-private.h:  Internal operations on handles
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#ifndef _WAPI_HANDLES_PRIVATE_H_
#define _WAPI_HANDLES_PRIVATE_H_

#include <config.h>
#include <glib.h>

#include <mono/io-layer/wapi-private.h>
#include <mono/io-layer/shared.h>

#undef DEBUG

/* Shared threads dont seem to work yet */
#undef _POSIX_THREAD_PROCESS_SHARED

extern struct _WapiHandleShared_list *_wapi_shared_data;
extern struct _WapiHandlePrivate_list *_wapi_private_data;

extern guint32 _wapi_handle_new_internal (WapiHandleType type);
extern gpointer _wapi_handle_new (WapiHandleType type);
extern gboolean _wapi_lookup_handle (gpointer handle, WapiHandleType type,
				     gpointer *shared, gpointer *private);
extern gpointer _wapi_search_handle (WapiHandleType type,
				     gboolean (*check)(gpointer, gpointer),
				     gpointer user_data,
				     gpointer *shared, gpointer *private);
extern void _wapi_handle_ref (gpointer handle);
extern void _wapi_handle_unref (gpointer handle);
extern guint32 _wapi_handle_scratch_store_internal (guint32 bytes);
extern guint32 _wapi_handle_scratch_store (gconstpointer data, guint32 bytes);
extern guint32 _wapi_handle_scratch_store_string_array (gchar **data);
extern gconstpointer _wapi_handle_scratch_lookup (guint32 idx);
extern guchar *_wapi_handle_scratch_lookup_as_string (guint32 idx);
extern gchar **_wapi_handle_scratch_lookup_string_array (guint32 idx);
extern void _wapi_handle_scratch_delete_internal (guint32 idx);
extern void _wapi_handle_scratch_delete (guint32 idx);
extern void _wapi_handle_scratch_delete_string_array (guint32 idx);
extern void _wapi_handle_register_capabilities (WapiHandleType type,
						WapiHandleCapability caps);
extern gboolean _wapi_handle_test_capabilities (gpointer handle,
						WapiHandleCapability caps);
extern void _wapi_handle_ops_close_shared (gpointer handle);
extern void _wapi_handle_ops_close_private (gpointer handle);
extern void _wapi_handle_ops_signal (gpointer handle);
extern void _wapi_handle_ops_own (gpointer handle);
extern gboolean _wapi_handle_ops_isowned (gpointer handle);

extern gboolean _wapi_handle_count_signalled_handles (guint32 numhandles,
						      gpointer *handles,
						      gboolean waitall,
						      guint32 *retcount,
						      guint32 *lowest);
extern void _wapi_handle_unlock_handles (guint32 numhandles,
					 gpointer *handles);
extern int _wapi_handle_wait_signal (void);
extern int _wapi_handle_timedwait_signal (struct timespec *timeout);
extern int _wapi_handle_wait_signal_handle (gpointer handle);
extern int _wapi_handle_timedwait_signal_handle (gpointer handle,
						 struct timespec *timeout);
extern gboolean _wapi_handle_process_fork (guint32 cmd, guint32 env,
					   guint32 dir, gboolean inherit,
					   guint32 flags,
					   gpointer stdin_handle,
					   gpointer stdout_handle,
					   gpointer stderr_handle,
					   gpointer *process_handle,
					   gpointer *thread_handle,
					   guint32 *pid, guint32 *tid);

static inline WapiHandleType _wapi_handle_type (gpointer handle)
{
	guint32 idx=GPOINTER_TO_UINT (handle);
	
	return(_wapi_shared_data->handles[idx].type);
}

static inline void _wapi_handle_set_signal_state (gpointer handle,
						  gboolean state,
						  gboolean broadcast)
{
	guint32 idx=GPOINTER_TO_UINT (handle);

#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": setting state of %p to %s (broadcast %s)", handle, state?"TRUE":"FALSE", broadcast?"TRUE":"FALSE");
#endif

	if(state==TRUE) {
		/* Tell everyone blocking on a single handle */

		/* This function _must_ be called with
		 * handle->signal_mutex locked
		 */
		_wapi_shared_data->handles[idx].signalled=state;
		
		if(broadcast==TRUE) {
			pthread_cond_broadcast (&_wapi_shared_data->handles[idx].signal_cond);
		} else {
			pthread_cond_signal (&_wapi_shared_data->handles[idx].signal_cond);
		}

		/* Tell everyone blocking on multiple handles that something
		 * was signalled
		 */
#if defined(_POSIX_THREAD_PROCESS_SHARED) && _POSIX_THREAD_PROCESS_SHARED != -1
		mono_mutex_lock (&_wapi_shared_data->signal_mutex);
		pthread_cond_broadcast (&_wapi_shared_data->signal_cond);
		mono_mutex_unlock (&_wapi_shared_data->signal_mutex);
#else
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": lock global signal mutex");
#endif

		mono_mutex_lock (&_wapi_private_data->signal_mutex);
		pthread_cond_broadcast (&_wapi_private_data->signal_cond);

#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unlock global signal mutex");
#endif

		mono_mutex_unlock (&_wapi_private_data->signal_mutex);
#endif /* _POSIX_THREAD_PROCESS_SHARED */
	} else {
		_wapi_shared_data->handles[idx].signalled=state;
	}
}

static inline gboolean _wapi_handle_issignalled (gpointer handle)
{
	guint32 idx=GPOINTER_TO_UINT (handle);

	return(_wapi_shared_data->handles[idx].signalled);
}

static inline int _wapi_handle_lock_signal_mutex (void)
{
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": lock global signal mutex");
#endif
#if defined(_POSIX_THREAD_PROCESS_SHARED) && _POSIX_THREAD_PROCESS_SHARED != -1
	return(mono_mutex_lock (&_wapi_shared_data->signal_mutex));
#else
	return(mono_mutex_lock (&_wapi_private_data->signal_mutex));
#endif /* _POSIX_THREAD_PROCESS_SHARED */
}

static inline int _wapi_handle_unlock_signal_mutex (void)
{
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": unlock global signal mutex");
#endif
#if defined(_POSIX_THREAD_PROCESS_SHARED) && _POSIX_THREAD_PROCESS_SHARED != -1
	return(mono_mutex_unlock (&_wapi_shared_data->signal_mutex));
#else
	return(mono_mutex_unlock (&_wapi_private_data->signal_mutex));
#endif /* _POSIX_THREAD_PROCESS_SHARED */
}

static inline int _wapi_handle_lock_handle (gpointer handle)
{
	guint32 idx=GPOINTER_TO_UINT (handle);

#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": locking handle %p", handle);
#endif
	
	return(mono_mutex_lock (&_wapi_shared_data->handles[idx].signal_mutex));
}

static inline int _wapi_handle_unlock_handle (gpointer handle)
{
	guint32 idx=GPOINTER_TO_UINT (handle);

#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": unlocking handle %p", handle);
#endif
	
	return(mono_mutex_unlock (&_wapi_shared_data->handles[idx].signal_mutex));
}

#endif /* _WAPI_HANDLES_PRIVATE_H_ */
