/*
 * threads-types.h: Generic thread typedef support (includes
 * system-specific files)
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2001 Ximian, Inc
 */

#ifndef _MONO_METADATA_THREADS_TYPES_H_
#define _MONO_METADATA_THREADS_TYPES_H_

#include <glib.h>

#include <mono/io-layer/io-layer.h>

/* This is a copy of System.Threading.ThreadState */
typedef enum {
	ThreadState_Running = 0x00000000,
	ThreadState_StopRequested = 0x00000001,
	ThreadState_SuspendRequested = 0x00000002,
	ThreadState_Background = 0x00000004,
	ThreadState_Unstarted = 0x00000008,
	ThreadState_Stopped = 0x00000010,
	ThreadState_WaitSleepJoin = 0x00000020,
	ThreadState_Suspended = 0x00000040,
	ThreadState_AbortRequested = 0x00000080,
	ThreadState_Aborted = 0x00000100
} MonoThreadState;

#define SPECIAL_STATIC_NONE 0
#define SPECIAL_STATIC_THREAD 1
#define SPECIAL_STATIC_CONTEXT 2

extern HANDLE ves_icall_System_Threading_Thread_Thread_internal(MonoThread *this_obj, MonoObject *start);
extern void ves_icall_System_Threading_Thread_Thread_free_internal(MonoThread *this_obj, HANDLE thread);
extern void ves_icall_System_Threading_Thread_Sleep_internal(int ms);
extern gboolean ves_icall_System_Threading_Thread_Join_internal(MonoThread *this_obj, int ms, HANDLE thread);
extern gint32 ves_icall_System_Threading_Thread_GetDomainID (void);
extern MonoString* ves_icall_System_Threading_Thread_GetName_internal (MonoThread *this_obj);
extern void ves_icall_System_Threading_Thread_SetName_internal (MonoThread *this_obj, MonoString *name);
extern MonoObject* ves_icall_System_Threading_Thread_GetCachedCurrentCulture (MonoThread *this_obj);
extern MonoArray* ves_icall_System_Threading_Thread_GetSerializedCurrentCulture (MonoThread *this_obj);
extern void ves_icall_System_Threading_Thread_SetCachedCurrentCulture (MonoThread *this_obj, MonoObject *culture);
void ves_icall_System_Threading_Thread_SetSerializedCurrentCulture (MonoThread *this_obj, MonoArray *arr);
extern MonoObject* ves_icall_System_Threading_Thread_GetCachedCurrentUICulture (MonoThread *this_obj);
extern MonoArray* ves_icall_System_Threading_Thread_GetSerializedCurrentUICulture (MonoThread *this_obj);
extern void ves_icall_System_Threading_Thread_SetCachedCurrentUICulture (MonoThread *this_obj, MonoObject *culture);
void ves_icall_System_Threading_Thread_SetSerializedCurrentUICulture (MonoThread *this_obj, MonoArray *arr);
extern HANDLE ves_icall_System_Threading_Mutex_CreateMutex_internal(MonoBoolean owned, MonoString *name, MonoBoolean *created);
extern void ves_icall_System_Threading_Mutex_ReleaseMutex_internal (HANDLE handle );
extern HANDLE ves_icall_System_Threading_Events_CreateEvent_internal (MonoBoolean manual, MonoBoolean initial, MonoString *name);
extern gboolean ves_icall_System_Threading_Events_SetEvent_internal (HANDLE handle);
extern gboolean ves_icall_System_Threading_Events_ResetEvent_internal (HANDLE handle);
extern void ves_icall_System_Threading_Events_CloseEvent_internal (HANDLE handle);

extern gboolean ves_icall_System_Threading_WaitHandle_WaitAll_internal(MonoArray *mono_handles, gint32 ms, gboolean exitContext);
extern gint32 ves_icall_System_Threading_WaitHandle_WaitAny_internal(MonoArray *mono_handles, gint32 ms, gboolean exitContext);
extern gboolean ves_icall_System_Threading_WaitHandle_WaitOne_internal(MonoObject *this_obj, HANDLE handle, gint32 ms, gboolean exitContext);

extern gint32 ves_icall_System_Threading_Interlocked_Increment_Int(gint32 *location);
extern gint64 ves_icall_System_Threading_Interlocked_Increment_Long(gint64 *location);
extern gint32 ves_icall_System_Threading_Interlocked_Decrement_Int(gint32 *location);
extern gint64 ves_icall_System_Threading_Interlocked_Decrement_Long(gint64 * location);

extern gint32 ves_icall_System_Threading_Interlocked_Exchange_Int(gint32 *location, gint32 value);
extern gint64 ves_icall_System_Threading_Interlocked_Exchange_Long(gint64 *location, gint64 value);
extern MonoObject *ves_icall_System_Threading_Interlocked_Exchange_Object(MonoObject **location, MonoObject *value);
extern gfloat ves_icall_System_Threading_Interlocked_Exchange_Single(gfloat *location, gfloat value);
extern gdouble ves_icall_System_Threading_Interlocked_Exchange_Double(gdouble *location, gdouble value);

extern gint32 ves_icall_System_Threading_Interlocked_CompareExchange_Int(gint32 *location, gint32 value, gint32 comparand);
extern gint64 ves_icall_System_Threading_Interlocked_CompareExchange_Long(gint64 *location, gint64 value, gint64 comparand);
extern MonoObject *ves_icall_System_Threading_Interlocked_CompareExchange_Object(MonoObject **location, MonoObject *value, MonoObject *comparand);
extern gfloat ves_icall_System_Threading_Interlocked_CompareExchange_Single(gfloat *location, gfloat value, gfloat comparand);
extern gdouble ves_icall_System_Threading_Interlocked_CompareExchange_Double(gdouble *location, gdouble value, gdouble comparand);
extern gpointer ves_icall_System_Threading_Interlocked_CompareExchange_T(gpointer *location, gpointer value, gpointer comparand);

extern gint32 ves_icall_System_Threading_Interlocked_Add_Int(gint32 *location, gint32 value);
extern gint64 ves_icall_System_Threading_Interlocked_Add_Long(gint64 *location, gint64 value);
extern gint64 ves_icall_System_Threading_Interlocked_Read_Long(gint64 *location);

extern gint32 ves_icall_System_Threading_Interlocked_Increment_Int(gint32 *location);
extern gint64 ves_icall_System_Threading_Interlocked_Increment_Long(gint64 *location);

extern gint32 ves_icall_System_Threading_Interlocked_Decrement_Int(gint32 *location);
extern gint64 ves_icall_System_Threading_Interlocked_Decrement_Long(gint64 * location);

extern void ves_icall_System_Threading_Thread_Abort (MonoThread *thread, MonoObject *state);
extern void ves_icall_System_Threading_Thread_ResetAbort (void);
extern void ves_icall_System_Threading_Thread_Suspend (MonoThread *thread);
extern void ves_icall_System_Threading_Thread_Resume (MonoThread *thread);
extern void ves_icall_System_Threading_Thread_ClrState (MonoThread *thread, guint32 state);
extern void ves_icall_System_Threading_Thread_SetState (MonoThread *thread, guint32 state);
extern guint32 ves_icall_System_Threading_Thread_GetState (MonoThread *thread);

gint8 ves_icall_System_Threading_Thread_VolatileRead1 (void *ptr);
gint16 ves_icall_System_Threading_Thread_VolatileRead2 (void *ptr);
gint32 ves_icall_System_Threading_Thread_VolatileRead4 (void *ptr);
gint64 ves_icall_System_Threading_Thread_VolatileRead8 (void *ptr);
void * ves_icall_System_Threading_Thread_VolatileReadIntPtr (void *ptr);

void ves_icall_System_Threading_Thread_VolatileWrite1 (void *ptr, gint8);
void ves_icall_System_Threading_Thread_VolatileWrite2 (void *ptr, gint16);
void ves_icall_System_Threading_Thread_VolatileWrite4 (void *ptr, gint32);
void ves_icall_System_Threading_Thread_VolatileWrite8 (void *ptr, gint64);
void ves_icall_System_Threading_Thread_VolatileWriteIntPtr (void *ptr, void *);

void ves_icall_System_Threading_Thread_MemoryBarrier (void);

#endif /* _MONO_METADATA_THREADS_TYPES_H_ */
