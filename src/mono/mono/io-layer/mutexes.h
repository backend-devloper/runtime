/*
 * mutexes.h: Mutex handles
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#ifndef _WAPI_MUTEXES_H_
#define _WAPI_MUTEXES_H_

#include <glib.h>

extern gpointer CreateMutex(WapiSecurityAttributes *security, gboolean owned,
			    const gunichar2 *name);
extern gboolean ReleaseMutex(gpointer handle);

#endif /* _WAPI_MUTEXES_H_ */
