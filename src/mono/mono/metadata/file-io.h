/*
 * file-io.h: File IO internal calls
 *
 * Authors:
 *	Dick Porter (dick@ximian.com)
 *	Dan Lewis (dihlewis@yahoo.co.uk)
 *
 * (C) 2001 Ximian, Inc.
 */

#ifndef _MONO_METADATA_FILEIO_H_
#define _MONO_METADATA_FILEIO_H_

#include <config.h>
#include <glib.h>

#include <mono/metadata/object-internals.h>
#include <mono/io-layer/io-layer.h>

/* This is a copy of System.IO.FileAccess */
typedef enum {
	FileAccess_Read=0x01,
	FileAccess_Write=0x02,
	FileAccess_ReadWrite=FileAccess_Read|FileAccess_Write
} MonoFileAccess;

/* This is a copy of System.IO.FileMode */
typedef enum {
	FileMode_CreateNew=1,
	FileMode_Create=2,
	FileMode_Open=3,
	FileMode_OpenOrCreate=4,
	FileMode_Truncate=5,
	FileMode_Append=6
} MonoFileMode;

/* This is a copy of System.IO.FileShare */
typedef enum {
	FileShare_None=0x0,
	FileShare_Read=0x01,
	FileShare_Write=0x02,
	FileShare_ReadWrite=FileShare_Read|FileShare_Write
} MonoFileShare;

/* This is a copy of System.IO.SeekOrigin */
typedef enum {
	SeekOrigin_Begin=0,
	SeekOrigin_Current=1,
	SeekOrigin_End=2
} MonoSeekOrigin;

/* This is a copy of System.IO.MonoIOStat */
typedef struct _MonoIOStat {
	MonoString *name;
	gint32 attributes;
	gint64 length;
	gint64 creation_time;
	gint64 last_access_time;
	gint64 last_write_time;
} MonoIOStat;

/* This is a copy of System.IO.FileAttributes */
typedef enum {
	FileAttributes_ReadOnly=0x00001,
	FileAttributes_Hidden=0x00002,
	FileAttributes_System=0x00004,
	FileAttributes_Directory=0x00010,
	FileAttributes_Archive=0x00020,
	FileAttributes_Device=0x00040,
	FileAttributes_Normal=0x00080,
	FileAttributes_Temporary=0x00100,
	FileAttributes_SparseFile=0x00200,
	FileAttributes_ReparsePoint=0x00400,
	FileAttributes_Compressed=0x00800,
	FileAttributes_Offline=0x01000,
	FileAttributes_NotContentIndexed=0x02000,
	FileAttributes_Encrypted=0x04000,
	FileAttributes_MonoExecutable= (int) 0x80000000
} MonoFileAttributes;

typedef struct _MonoFSAsyncResult {
	MonoObject obj;
	MonoObject *state;
	MonoBoolean completed;
	MonoBoolean done;
	MonoException *exc;
	MonoWaitHandle *wait_handle;
	MonoDelegate *async_callback;
	MonoBoolean completed_synch;
	MonoArray *buffer;
	gint offset;
	gint count;
	gint original_count;
	gint bytes_read;
	MonoDelegate *real_cb;
} MonoFSAsyncResult;

#ifdef PLATFORM_WIN32
typedef struct _WapiOverlapped WapiOverlapped;

struct _WapiOverlapped
{
	guint32 Internal;
	guint32 InternalHigh;
	guint32 Offset;
	guint32 OffsetHigh;
	gpointer hEvent;
	gpointer handle1;
	gpointer handle2;
};
#endif

/* System.IO.MonoIO */

extern MonoBoolean
ves_icall_System_IO_MonoIO_CreateDirectory (MonoString *path, gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_RemoveDirectory (MonoString *path, gint32 *error);

extern HANDLE 
ves_icall_System_IO_MonoIO_FindFirstFile (MonoString *path, MonoIOStat *stat,
					  gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_FindNextFile (HANDLE find, MonoIOStat *stat,
					 gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_FindClose (HANDLE find, gint32 *error);

extern MonoString *
ves_icall_System_IO_MonoIO_GetCurrentDirectory (gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_SetCurrentDirectory (MonoString *path,
						gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_MoveFile (MonoString *path, MonoString *dest,
				     gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_CopyFile (MonoString *path, MonoString *dest,
				     MonoBoolean overwrite, gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_DeleteFile (MonoString *path, gint32 *error);

extern gint32 
ves_icall_System_IO_MonoIO_GetFileAttributes (MonoString *path, gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_SetFileAttributes (MonoString *path, gint32 attrs,
					      gint32 *error);

extern gint32
ves_icall_System_IO_MonoIO_GetFileType (HANDLE handle, gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_GetFileStat (MonoString *path, MonoIOStat *stat,
					gint32 *error);

extern HANDLE 
ves_icall_System_IO_MonoIO_Open (MonoString *filename, gint32 mode,
				 gint32 access_mode, gint32 share,
				 MonoBoolean async, gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_Close (HANDLE handle, gint32 *error);

extern gint32 
ves_icall_System_IO_MonoIO_Read (HANDLE handle, MonoArray *dest,
				 gint32 dest_offset, gint32 count,
				 gint32 *error);

extern gint32 
ves_icall_System_IO_MonoIO_Write (HANDLE handle, MonoArray *src,
				  gint32 src_offset, gint32 count,
				  gint32 *error);

extern gint64 
ves_icall_System_IO_MonoIO_Seek (HANDLE handle, gint64 offset, gint32 origin,
				 gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_Flush (HANDLE handle, gint32 *error);

extern gint64 
ves_icall_System_IO_MonoIO_GetLength (HANDLE handle, gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_SetLength (HANDLE handle, gint64 length,
				      gint32 *error);

extern MonoBoolean
ves_icall_System_IO_MonoIO_SetFileTime (HANDLE handle, gint64 creation_time,
					gint64 last_access_time,
					gint64 last_write_time, gint32 *error);

extern HANDLE 
ves_icall_System_IO_MonoIO_get_ConsoleOutput (void);

extern HANDLE 
ves_icall_System_IO_MonoIO_get_ConsoleInput (void);

extern HANDLE 
ves_icall_System_IO_MonoIO_get_ConsoleError (void);

extern MonoBoolean
ves_icall_System_IO_MonoIO_CreatePipe (HANDLE *read_handle,
				       HANDLE *write_handle);

extern gunichar2 
ves_icall_System_IO_MonoIO_get_VolumeSeparatorChar (void);

extern gunichar2 
ves_icall_System_IO_MonoIO_get_DirectorySeparatorChar (void);

extern gunichar2 
ves_icall_System_IO_MonoIO_get_AltDirectorySeparatorChar (void);

extern gunichar2 
ves_icall_System_IO_MonoIO_get_PathSeparator (void);

extern MonoArray *
ves_icall_System_IO_MonoIO_get_InvalidPathChars (void);

extern gint32
ves_icall_System_IO_MonoIO_GetTempPath (MonoString **mono_name);

extern MonoBoolean
ves_icall_System_IO_MonoIO_GetSupportsAsync (void);

extern MonoBoolean
ves_icall_System_IO_MonoIO_BeginRead (HANDLE handle, MonoFSAsyncResult *ares);

extern MonoBoolean
ves_icall_System_IO_MonoIO_BeginWrite (HANDLE handle, MonoFSAsyncResult *ares);

extern void ves_icall_System_IO_MonoIO_Lock (HANDLE handle, gint64 position,
					     gint64 length, gint32 *error);
extern void ves_icall_System_IO_MonoIO_Unlock (HANDLE handle, gint64 position,
					       gint64 length, gint32 *error);

#endif /* _MONO_METADATA_FILEIO_H_ */
