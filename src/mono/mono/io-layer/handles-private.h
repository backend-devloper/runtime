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
#include <errno.h>
#include <signal.h>
#include <string.h>

#include <mono/io-layer/wapi-private.h>
#include <mono/io-layer/misc-private.h>
#include <mono/io-layer/collection.h>

#undef DEBUG

extern struct _WapiHandleUnshared *_wapi_private_handles;
extern struct _WapiHandleSharedLayout *_wapi_shared_layout;
extern struct _WapiFileShareLayout *_wapi_fileshare_layout;

extern guint32 _wapi_fd_reserve;
extern mono_mutex_t _wapi_global_signal_mutex;
extern pthread_cond_t _wapi_global_signal_cond;

extern gpointer _wapi_handle_new (WapiHandleType type,
				  gpointer handle_specific);
extern gpointer _wapi_handle_new_for_existing_ns (WapiHandleType type,
						  gpointer handle_specific,
						  guint32 offset);
extern gpointer _wapi_handle_new_fd (WapiHandleType type, int fd,
				     gpointer handle_specific);
extern gboolean _wapi_lookup_handle (gpointer handle, WapiHandleType type,
				     gpointer *handle_specific);
extern gboolean _wapi_copy_handle (gpointer handle, WapiHandleType type,
				   struct _WapiHandleShared *handle_specific);
extern void _wapi_replace_handle (gpointer handle, WapiHandleType type,
				  struct _WapiHandleShared *handle_specific);
extern gboolean _wapi_try_replace_handle (gpointer handle, WapiHandleType type, struct _WapiHandleShared *handle_specific);
extern gpointer _wapi_search_handle (WapiHandleType type,
				     gboolean (*check)(gpointer, gpointer),
				     gpointer user_data,
				     gpointer *handle_specific);
extern int _wapi_namespace_timestamp_release (gpointer nowptr);
extern int _wapi_namespace_timestamp (guint32 now);
extern gint32 _wapi_search_handle_namespace (WapiHandleType type,
					     gchar *utf8_name);
extern void _wapi_handle_ref (gpointer handle);
extern void _wapi_handle_unref (gpointer handle);
extern void _wapi_handle_register_capabilities (WapiHandleType type,
						WapiHandleCapability caps);
extern gboolean _wapi_handle_test_capabilities (gpointer handle,
						WapiHandleCapability caps);
extern void _wapi_handle_ops_close (gpointer handle);
extern void _wapi_handle_ops_signal (gpointer handle);
extern gboolean _wapi_handle_ops_own (gpointer handle);
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
extern int _wapi_handle_wait_signal_poll_share (void);
extern int _wapi_handle_timedwait_signal_poll_share (struct timespec *timeout);
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

extern gboolean _wapi_handle_process_kill (pid_t pid, guint32 signo,
					   gint *err);
extern gboolean _wapi_handle_get_or_set_share (dev_t device, ino_t inode,
					       guint32 new_sharemode,
					       guint32 new_access,
					       guint32 *old_sharemode,
					       guint32 *old_access,
					       struct _WapiFileShare **info);
extern void _wapi_handle_check_share (struct _WapiFileShare *share_info);
extern void _wapi_handle_dump (void);
extern void _wapi_handle_update_refs (void);

/* This is OK to use for atomic writes of individual struct members, as they
 * are independent
 */
#define WAPI_SHARED_HANDLE_METADATA(handle) _wapi_shared_layout->metadata[_wapi_private_handles[GPOINTER_TO_UINT((handle))].u.shared.offset]

/* Use this for read-only accesses to shared handle structs, so that
 * if a handle is updated we always look at the latest copy.  For
 * write access, use handle_copy/handle_replace so that all the
 * handle-type-specific data is updated atomically.
 */
#define WAPI_SHARED_HANDLE_TYPED_DATA(handle, type) _wapi_shared_layout->handles[_wapi_shared_layout->metadata[_wapi_private_handles[GPOINTER_TO_UINT((handle))].u.shared.offset].offset].u.type

static inline WapiHandleType _wapi_handle_type (gpointer handle)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	
	return(_wapi_private_handles[idx].type);
}

static inline void _wapi_handle_set_signal_state (gpointer handle,
						  gboolean state,
						  gboolean broadcast)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	struct _WapiHandleUnshared *handle_data;
	int thr_ret;

	g_assert (!_WAPI_SHARED_HANDLE(_wapi_handle_type (handle)));
	
	handle_data = &_wapi_private_handles[idx];
	
#ifdef DEBUG
	g_message ("%s: setting state of %p to %s (broadcast %s)", __func__,
		   handle, state?"TRUE":"FALSE", broadcast?"TRUE":"FALSE");
#endif

	if (state == TRUE) {
		/* Tell everyone blocking on a single handle */

		/* This function _must_ be called with
		 * handle->signal_mutex locked
		 */
		handle_data->signalled=state;
		
		if (broadcast == TRUE) {
			thr_ret = pthread_cond_broadcast (&handle_data->signal_cond);
			g_assert (thr_ret == 0);
		} else {
			thr_ret = pthread_cond_signal (&handle_data->signal_cond);
			g_assert (thr_ret == 0);
		}

		/* Tell everyone blocking on multiple handles that something
		 * was signalled
		 */
		pthread_cleanup_push ((void(*)(void *))mono_mutex_unlock_in_cleanup, (void *)&_wapi_global_signal_mutex);
		thr_ret = mono_mutex_lock (&_wapi_global_signal_mutex);
		g_assert (thr_ret == 0);
			
		thr_ret = pthread_cond_broadcast (&_wapi_global_signal_cond);
		g_assert (thr_ret == 0);
			
		thr_ret = mono_mutex_unlock (&_wapi_global_signal_mutex);
		g_assert (thr_ret == 0);
		pthread_cleanup_pop (0);
	} else {
		handle_data->signalled=state;
	}
}

static inline void _wapi_shared_handle_set_signal_state (gpointer handle,
							 gboolean state)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	struct _WapiHandleUnshared *handle_data;
	struct _WapiHandle_shared_ref *ref;
	struct _WapiHandleSharedMetadata *shared_meta;
	
	g_assert (_WAPI_SHARED_HANDLE(_wapi_handle_type (handle)));
	
	handle_data = &_wapi_private_handles[idx];
	
	ref = &handle_data->u.shared;
	shared_meta = &_wapi_shared_layout->metadata[ref->offset];
	shared_meta->signalled = state;

#ifdef DEBUG
	g_message ("%s: signalled shared handle offset 0x%x", __func__,
		   ref->offset);
#endif

	InterlockedIncrement (&_wapi_shared_layout->signal_count);
}

static inline gboolean _wapi_handle_issignalled (gpointer handle)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	
	if (_WAPI_SHARED_HANDLE(_wapi_handle_type (handle))) {
		return(WAPI_SHARED_HANDLE_METADATA(handle).signalled);
	} else {
		return(_wapi_private_handles[idx].signalled);
	}
}

static inline int _wapi_handle_lock_signal_mutex (void)
{
#ifdef DEBUG
	g_message ("%s: lock global signal mutex", __func__);
#endif

	return(mono_mutex_lock (&_wapi_global_signal_mutex));
}

/* the parameter makes it easier to call from a pthread cleanup handler */
static inline int _wapi_handle_unlock_signal_mutex (void *unused)
{
#ifdef DEBUG
	g_message ("%s: unlock global signal mutex", __func__);
#endif

	return(mono_mutex_unlock (&_wapi_global_signal_mutex));
}

static inline int _wapi_handle_lock_handle (gpointer handle)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	
#ifdef DEBUG
	g_message ("%s: locking handle %p", __func__, handle);
#endif

	_wapi_handle_ref (handle);
	
	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		return(0);
	}
	
	return(mono_mutex_lock (&_wapi_private_handles[idx].signal_mutex));
}

static inline int _wapi_handle_trylock_handle (gpointer handle)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	
#ifdef DEBUG
	g_message ("%s: locking handle %p", __func__, handle);
#endif

	_wapi_handle_ref (handle);
	
	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		return(0);
	}

	return(mono_mutex_trylock (&_wapi_private_handles[idx].signal_mutex));
}

static inline int _wapi_handle_unlock_handle (gpointer handle)
{
	guint32 idx = GPOINTER_TO_UINT(handle);
	int ret;
	
#ifdef DEBUG
	g_message ("%s: unlocking handle %p", __func__, handle);
#endif
	
	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		_wapi_handle_unref (handle);
		return(0);
	}
	
	ret = mono_mutex_unlock (&_wapi_private_handles[idx].signal_mutex);

	_wapi_handle_unref (handle);
	
	return(ret);
}

static inline int _wapi_timestamp_exclusion (volatile gint32 *timestamp,
					     guint32 now)
{
	guint32 then;
	int ret;

	then = InterlockedCompareExchange (timestamp, now, 0);
	if (then == 0) {
		ret = 0;
	} else if (now - then > 10) {
		/* Try to overwrite the previous
		 * attempt, but make sure noone else
		 * got in first
		 */
		g_warning ("%s: Breaking a previous timestamp", __func__);
				
		ret = InterlockedCompareExchange (timestamp, now,
						  then) == then?0:EBUSY;
	} else {
		/* Someone else is working on this one */
		ret = EBUSY;
	}
			
	return(ret);
}

static inline int _wapi_timestamp_release (volatile gint32 *timestamp,
					   guint32 now)
{
	/* The timestamp can be either: now, in which case we reset
	 * it; 0, in which case we don't do anything; any other value,
	 * in which case we don't do anything because someone else is
	 * in charge of resetting it.
	 */
	return(InterlockedCompareExchange (timestamp, 0, now) != now);
}

static inline void _wapi_handle_spin (guint32 ms)
{
	struct timespec sleepytime;
	
	sleepytime.tv_sec = 0;
	sleepytime.tv_nsec = ms * 1000000;
	
	nanosleep (&sleepytime, NULL);
}

static inline void _wapi_handle_share_release (struct _WapiFileShare *info)
{
	guint32 now = (guint32)(time(NULL) & 0xFFFFFFFF);
	int thr_ret;
	
	g_assert (info->handle_refs > 0);
	
	/* Marking this as COLLECTION_UNSAFE prevents entries from
	 * expiring under us if we remove this one
	 */
	_WAPI_HANDLE_COLLECTION_UNSAFE;
	
	/* Prevent new entries racing with us */
	thr_ret = _wapi_timestamp_exclusion (&_wapi_fileshare_layout->share_check, now);
	g_assert (thr_ret == 0);

	if (InterlockedDecrement (&info->handle_refs) == 0) {
		memset (info, '\0', sizeof(struct _WapiFileShare));
	}

	thr_ret = _wapi_timestamp_release (&_wapi_fileshare_layout->share_check, now);
	g_assert (thr_ret == 0);

	_WAPI_HANDLE_COLLECTION_SAFE;
}

#endif /* _WAPI_HANDLES_PRIVATE_H_ */
