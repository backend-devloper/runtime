/*
 * wapi-private.h:  internal definitions of handles and shared memory layout
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002-2006 Novell, Inc.
 */

#ifndef _WAPI_PRIVATE_H_
#define _WAPI_PRIVATE_H_

#include <config.h>
#include <glib.h>
#include <sys/stat.h>

#include <mono/io-layer/wapi.h>
#include <mono/io-layer/handles.h>
#include <mono/io-layer/io.h>

#include <mono/utils/mono-os-mutex.h>

extern gboolean _wapi_has_shut_down;

typedef enum {
	WAPI_HANDLE_UNUSED=0,
	WAPI_HANDLE_FILE,
	WAPI_HANDLE_CONSOLE,
	WAPI_HANDLE_THREAD,
	WAPI_HANDLE_SEM,
	WAPI_HANDLE_MUTEX,
	WAPI_HANDLE_EVENT,
	WAPI_HANDLE_SOCKET,
	WAPI_HANDLE_FIND,
	WAPI_HANDLE_PROCESS,
	WAPI_HANDLE_PIPE,
	WAPI_HANDLE_NAMEDMUTEX,
	WAPI_HANDLE_NAMEDSEM,
	WAPI_HANDLE_NAMEDEVENT,
	WAPI_HANDLE_COUNT
} WapiHandleType;

extern const char *_wapi_handle_typename[];

#define _WAPI_FD_HANDLE(type) (type == WAPI_HANDLE_FILE || \
			       type == WAPI_HANDLE_CONSOLE || \
			       type == WAPI_HANDLE_SOCKET || \
			       type == WAPI_HANDLE_PIPE)

#define _WAPI_SHARED_NAMESPACE(type) (type == WAPI_HANDLE_NAMEDMUTEX || \
				      type == WAPI_HANDLE_NAMEDSEM || \
				      type == WAPI_HANDLE_NAMEDEVENT)

typedef struct 
{
	gchar name[MAX_PATH + 1];
} WapiSharedNamespace;

typedef enum {
	WAPI_HANDLE_CAP_WAIT=0x01,
	WAPI_HANDLE_CAP_SIGNAL=0x02,
	WAPI_HANDLE_CAP_OWN=0x04,
	WAPI_HANDLE_CAP_SPECIAL_WAIT=0x08
} WapiHandleCapability;

struct _WapiHandleOps 
{
	void (*close)(gpointer handle, gpointer data);

	/* SignalObjectAndWait */
	void (*signal)(gpointer signal);

	/* Called by WaitForSingleObject and WaitForMultipleObjects,
	 * with the handle locked (shared handles aren't locked.)
	 * Returns TRUE if ownership was established, false otherwise.
	 */
	gboolean (*own_handle)(gpointer handle);

	/* Called by WaitForSingleObject and WaitForMultipleObjects, if the
	 * handle in question is "ownable" (ie mutexes), to see if the current
	 * thread already owns this handle
	 */
	gboolean (*is_owned)(gpointer handle);

	/* Called by WaitForSingleObject and WaitForMultipleObjects,
	 * if the handle in question needs a special wait function
	 * instead of using the normal handle signal mechanism.
	 * Returns the WaitForSingleObject return code.
	 */
	guint32 (*special_wait)(gpointer handle, guint32 timeout, gboolean alertable);

	/* Called by WaitForSingleObject and WaitForMultipleObjects,
	 * if the handle in question needs some preprocessing before the
	 * signal wait.
	 */
	void (*prewait)(gpointer handle);

	/* Called when dumping the handles */
	void (*details)(gpointer data);

	/* Called to get the name of the handle type */
	const gchar* (*typename) (void);

	/* Called to get the size of the handle type */
	gsize (*typesize) (void);
};

#include <mono/io-layer/event-private.h>
#include <mono/io-layer/io-private.h>
#include <mono/io-layer/mutex-private.h>
#include <mono/io-layer/semaphore-private.h>
#include <mono/io-layer/socket-private.h>
#include <mono/io-layer/thread-private.h>
#include <mono/io-layer/process-private.h>

struct _WapiHandle_shared_ref
{
	/* This will be split 16:16 with the shared file segment in
	 * the top half, when I implement space increases
	 */
	guint32 offset;
};

#define _WAPI_SHARED_SEM_NAMESPACE 0
/*#define _WAPI_SHARED_SEM_COLLECTION 1*/
#define _WAPI_SHARED_SEM_FILESHARE 2
#define _WAPI_SHARED_SEM_PROCESS_COUNT_LOCK 6
#define _WAPI_SHARED_SEM_PROCESS_COUNT 7
#define _WAPI_SHARED_SEM_COUNT 8	/* Leave some future expansion space */

struct _WapiFileShare
{
#ifdef WAPI_FILE_SHARE_PLATFORM_EXTRA_DATA
	WAPI_FILE_SHARE_PLATFORM_EXTRA_DATA
#endif
	guint64 device;
	guint64 inode;
	pid_t opened_by_pid;
	guint32 sharemode;
	guint32 access;
	guint32 handle_refs;
	guint32 timestamp;
};

typedef struct _WapiFileShare _WapiFileShare;

#define _WAPI_HANDLE_INVALID (gpointer)-1

pid_t
_wapi_getpid (void);

#endif /* _WAPI_PRIVATE_H_ */
