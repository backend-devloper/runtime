/*
 * process-private.h: Private definitions for process handles
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002-2006 Novell, Inc.
 */

#ifndef _WAPI_PROCESS_PRIVATE_H_
#define _WAPI_PROCESS_PRIVATE_H_

#include <config.h>
#include <glib.h>

/* There doesn't seem to be a defined symbol for this */
#define _WAPI_PROCESS_CURRENT (gpointer)0xFFFFFFFF

/* This marks a system process that we don't have a handle on */
/* FIXME: Cope with PIDs > sizeof guint */
#define _WAPI_PROCESS_UNHANDLED (1 << (8*sizeof(pid_t)-1))
#define _WAPI_PROCESS_UNHANDLED_PID_MASK (-1 & ~_WAPI_PROCESS_UNHANDLED)

extern gpointer _wapi_process_duplicate (void);

extern struct _WapiHandleOps _wapi_process_ops;

#define _WAPI_PROC_NAME_MAX_LEN _POSIX_PATH_MAX

struct _WapiHandle_process
{
	pid_t id;
	guint32 exitstatus;
	gpointer main_thread;
	WapiFileTime create_time;
	WapiFileTime exit_time;
	gchar proc_name[_WAPI_PROC_NAME_MAX_LEN];
	size_t min_working_set;
	size_t max_working_set;
	gboolean waited;
};

extern void _wapi_process_reap (void);
extern void _wapi_process_signal_self (void);

#endif /* _WAPI_PROCESS_PRIVATE_H_ */
