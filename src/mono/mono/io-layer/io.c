/*
 * io.c:  File, console and find handles
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <utime.h>

#ifndef PLATFORM_WIN32
#ifdef HAVE_AIO_H
#include <aio.h>
#define USE_AIO	1
#elif defined(HAVE_SYS_AIO_H)
#include <sys/aio.h>
#define USE_AIO 1
#else
#undef USE_AIO
#endif
#endif

#include <mono/io-layer/wapi.h>
#include <mono/io-layer/wapi-private.h>
#include <mono/io-layer/handles-private.h>
#include <mono/io-layer/io-private.h>
#include <mono/io-layer/timefuncs-private.h>
#include <mono/utils/strenc.h>

#undef DEBUG

static void file_close_shared (gpointer handle);
static void file_close_private (gpointer handle);
static WapiFileType file_getfiletype(void);
static gboolean file_read(gpointer handle, gpointer buffer,
			  guint32 numbytes, guint32 *bytesread,
			  WapiOverlapped *overlapped);
static gboolean file_write(gpointer handle, gconstpointer buffer,
			   guint32 numbytes, guint32 *byteswritten,
			   WapiOverlapped *overlapped);
static gboolean file_flush(gpointer handle);
static guint32 file_seek(gpointer handle, gint32 movedistance,
			 gint32 *highmovedistance, WapiSeekMethod method);
static gboolean file_setendoffile(gpointer handle);
static guint32 file_getfilesize(gpointer handle, guint32 *highsize);
static gboolean file_getfiletime(gpointer handle, WapiFileTime *create_time,
				 WapiFileTime *last_access,
				 WapiFileTime *last_write);
static gboolean file_setfiletime(gpointer handle,
				 const WapiFileTime *create_time,
				 const WapiFileTime *last_access,
				 const WapiFileTime *last_write);

/* File handle is only signalled for overlapped IO */
struct _WapiHandleOps _wapi_file_ops = {
	file_close_shared,	/* close_shared */
	file_close_private,	/* close_private */
	NULL,			/* signal */
	NULL,			/* own */
	NULL,			/* is_owned */
};

static void console_close_shared (gpointer handle);
static void console_close_private (gpointer handle);
static WapiFileType console_getfiletype(void);
static gboolean console_read(gpointer handle, gpointer buffer,
			     guint32 numbytes, guint32 *bytesread,
			     WapiOverlapped *overlapped);
static gboolean console_write(gpointer handle, gconstpointer buffer,
			      guint32 numbytes, guint32 *byteswritten,
			      WapiOverlapped *overlapped);

/* Console is mostly the same as file, except it can block waiting for
 * input or output
 */
struct _WapiHandleOps _wapi_console_ops = {
	console_close_shared,	/* close_shared */
	console_close_private,	/* close_private */
	NULL,			/* signal */
	NULL,			/* own */
	NULL,			/* is_owned */
};

/* Find handle has no ops.
 */
struct _WapiHandleOps _wapi_find_ops = {
	NULL,			/* close_shared */
	NULL,			/* close_private */
	NULL,			/* signal */
	NULL,			/* own */
	NULL,			/* is_owned */
};

static void pipe_close_shared (gpointer handle);
static void pipe_close_private (gpointer handle);
static WapiFileType pipe_getfiletype (void);
static gboolean pipe_read (gpointer handle, gpointer buffer, guint32 numbytes,
			   guint32 *bytesread, WapiOverlapped *overlapped);
static gboolean pipe_write (gpointer handle, gconstpointer buffer,
			    guint32 numbytes, guint32 *byteswritten,
			    WapiOverlapped *overlapped);

/* Pipe handles
 */
struct _WapiHandleOps _wapi_pipe_ops = {
	pipe_close_shared,	/* close_shared */
	pipe_close_private,	/* close_private */
	NULL,			/* signal */
	NULL,			/* own */
	NULL,			/* is_owned */
};

static struct {
	/* File, console and pipe handles */
	WapiFileType (*getfiletype)(void);
	
	/* File, console and pipe handles */
	gboolean (*readfile)(gpointer handle, gpointer buffer,
			     guint32 numbytes, guint32 *bytesread,
			     WapiOverlapped *overlapped);
	gboolean (*writefile)(gpointer handle, gconstpointer buffer,
			      guint32 numbytes, guint32 *byteswritten,
			      WapiOverlapped *overlapped);
	gboolean (*flushfile)(gpointer handle);
	
	/* File handles */
	guint32 (*seek)(gpointer handle, gint32 movedistance,
			gint32 *highmovedistance, WapiSeekMethod method);
	gboolean (*setendoffile)(gpointer handle);
	guint32 (*getfilesize)(gpointer handle, guint32 *highsize);
	gboolean (*getfiletime)(gpointer handle, WapiFileTime *create_time,
				WapiFileTime *last_access,
				WapiFileTime *last_write);
	gboolean (*setfiletime)(gpointer handle,
				const WapiFileTime *create_time,
				const WapiFileTime *last_access,
				const WapiFileTime *last_write);
} io_ops[WAPI_HANDLE_COUNT]={
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* file */
	{file_getfiletype,
	 file_read, file_write,
	 file_flush, file_seek,
	 file_setendoffile,
	 file_getfilesize,
	 file_getfiletime,
	 file_setfiletime},
	/* console */
	{console_getfiletype,
	 console_read,
	 console_write,
	 NULL, NULL, NULL, NULL, NULL, NULL},
	/* thread */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* sem */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* mutex */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* event */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* socket (will need at least read and write) */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* find */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* process */
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
	/* pipe */
	{pipe_getfiletype,
	 pipe_read,
	 pipe_write,
	 NULL, NULL, NULL, NULL, NULL, NULL},
};


static mono_once_t io_ops_once=MONO_ONCE_INIT;

static void io_ops_init (void)
{
/* 	_wapi_handle_register_capabilities (WAPI_HANDLE_FILE, */
/* 					    WAPI_HANDLE_CAP_WAIT); */
/* 	_wapi_handle_register_capabilities (WAPI_HANDLE_CONSOLE, */
/* 					    WAPI_HANDLE_CAP_WAIT); */
}

/* Some utility functions.
 */

static guint32 _wapi_stat_to_file_attributes (struct stat *buf)
{
	guint32 attrs = 0;

	/* FIXME: this could definitely be better */

	if (S_ISDIR (buf->st_mode))
		attrs |= FILE_ATTRIBUTE_DIRECTORY;
	else
		attrs |= FILE_ATTRIBUTE_ARCHIVE;
	
	if (!(buf->st_mode & S_IWUSR))
		attrs |= FILE_ATTRIBUTE_READONLY;
	
	return attrs;
}

static void
_wapi_set_last_error_from_errno (void)
{
	SetLastError (_wapi_get_win32_file_error (errno));
}

/* Handle ops.
 */
static void file_close_shared (gpointer handle)
{
	struct _WapiHandle_file *file_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle, NULL);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return;
	}
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": closing file handle %p", handle);
#endif
	
	if(file_handle->filename!=0) {
		_wapi_handle_scratch_delete (file_handle->filename);
		file_handle->filename=0;
	}
	if(file_handle->security_attributes!=0) {
		_wapi_handle_scratch_delete (file_handle->security_attributes);
		file_handle->security_attributes=0;
	}
}

static void file_close_private (gpointer handle)
{
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE, NULL,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return;
	}
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": closing file handle %p with fd %d",
		  handle, file_private_handle->fd);
#endif
	
	close(file_private_handle->fd);
}

static WapiFileType file_getfiletype(void)
{
	return(FILE_TYPE_DISK);
}

#ifdef USE_AIO
typedef struct {
	struct aiocb *aio;
	WapiOverlapped *overlapped;
	WapiOverlappedCB callback;
} notifier_data_t;

#define SIGPTR(a) a.SIGVAL_PTR

static void
async_notifier (union sigval sig)
{
	notifier_data_t *ndata = SIGPTR (sig);
	guint32 error;
	guint32 numbytes;

	error = aio_return (ndata->aio);
	if (error < 0) {
		error = _wapi_get_win32_file_error (error);
		numbytes = 0;
	} else {
		numbytes = error;
		error = 0;
	}

	ndata->callback (error, numbytes, ndata->overlapped);
	g_free (ndata->aio);
	g_free (ndata);
}

#endif /* USE_AIO */

static gboolean file_read(gpointer handle, gpointer buffer,
			  guint32 numbytes, guint32 *bytesread,
			  WapiOverlapped *overlapped)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(FALSE);
	}
	
	if(bytesread!=NULL) {
		*bytesread=0;
	}
	
	if(!(file_handle->fileaccess&GENERIC_READ) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION": handle %p fd %d doesn't have GENERIC_READ access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(FALSE);
	}

	if (file_private_handle->async == FALSE) {
		ret=read(file_private_handle->fd, buffer, numbytes);
		if(ret==-1) {
			gint err = errno;

#ifdef DEBUG
			g_message(G_GNUC_PRETTY_FUNCTION
				  ": read of handle %p fd %d error: %s", handle,
				  file_private_handle->fd, strerror(err));
#endif
			SetLastError (_wapi_get_win32_file_error (err));
			return(FALSE);
		}
		
		if(bytesread!=NULL) {
			*bytesread=ret;
		}
		
		return(TRUE);
	}

#ifndef USE_AIO
	SetLastError (ERROR_NOT_SUPPORTED);
	return FALSE;
#else
	if (overlapped == NULL || file_private_handle->callback == NULL) {
		SetLastError (ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	{
	int fd = file_private_handle->fd;
	struct aiocb *aio;
	int result;
	notifier_data_t *ndata;

	ndata = g_new0 (notifier_data_t, 1);
	aio = g_new0 (struct aiocb, 1);
	ndata->overlapped = overlapped;
	ndata->aio = aio;
	ndata->callback = file_private_handle->callback;

	aio->aio_fildes = fd;
	aio->aio_lio_opcode = LIO_READ;
	aio->aio_nbytes = numbytes;
	aio->aio_offset = overlapped->Offset + (((gint64) overlapped->OffsetHigh) << 32);
	aio->aio_buf = buffer;
	aio->aio_sigevent.sigev_notify = SIGEV_THREAD;
	aio->aio_sigevent.sigev_notify_function = async_notifier;
	SIGPTR (aio->aio_sigevent.sigev_value) = ndata;

	result = aio_read (aio);
	if (result == -1) {
		_wapi_set_last_error_from_errno ();
		return FALSE;
	}

	result = aio_error (aio);
#ifdef DEBUG
	g_print ("aio_error (read) returned %d for %d\n", result, fd);
#endif
	if (result == 0) {
		numbytes = aio_return (aio);
#ifdef DEBUG
		g_print ("numbytes %d for %d\n", numbytes, fd);
#endif
	} else {
		errno = result;
		_wapi_set_last_error_from_errno ();
		return FALSE;
	}

	if (bytesread)
		*bytesread = numbytes;

	return TRUE;
	}
#endif
}

static gboolean file_write(gpointer handle, gconstpointer buffer,
			   guint32 numbytes, guint32 *byteswritten,
			   WapiOverlapped *overlapped G_GNUC_UNUSED)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(FALSE);
	}
	
	if(byteswritten!=NULL) {
		*byteswritten=0;
	}
	
	if(!(file_handle->fileaccess&GENERIC_WRITE) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_WRITE access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(FALSE);
	}
	
	if (file_private_handle->async == FALSE) {
		ret=write(file_private_handle->fd, buffer, numbytes);
		if(ret==-1) {
#ifdef DEBUG
			g_message(G_GNUC_PRETTY_FUNCTION
				  ": write of handle %p fd %d error: %s", handle,
				  file_private_handle->fd, strerror(errno));
#endif

			return(FALSE);
		}
		if(byteswritten!=NULL) {
			*byteswritten=ret;
		}
		return(TRUE);
	}

#ifndef USE_AIO
	SetLastError (ERROR_NOT_SUPPORTED);
	return FALSE;
#else
	if (overlapped == NULL || file_private_handle->callback == NULL) {
		SetLastError (ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	{
	int fd = file_private_handle->fd;
	struct aiocb *aio;
	int result;
	notifier_data_t *ndata;

	ndata = g_new0 (notifier_data_t, 1);
	aio = g_new0 (struct aiocb, 1);
	ndata->overlapped = overlapped;
	ndata->aio = aio;
	ndata->callback = file_private_handle->callback;

	aio->aio_fildes = fd;
	aio->aio_lio_opcode = LIO_WRITE;
	aio->aio_nbytes = numbytes;
	aio->aio_offset = overlapped->Offset + (((gint64) overlapped->OffsetHigh) << 32);
	aio->aio_buf = (gpointer) buffer;
	aio->aio_sigevent.sigev_notify = SIGEV_THREAD;
	aio->aio_sigevent.sigev_notify_function = async_notifier;
	SIGPTR (aio->aio_sigevent.sigev_value) = ndata;

	result = aio_write (aio);
	if (result == -1) {
		_wapi_set_last_error_from_errno ();
		return FALSE;
	}

	result = aio_error (aio);
#ifdef DEBUG
	g_print ("aio_error (write) returned %d for %d\n", result, fd);
#endif
	if (result == 0) {
		numbytes = aio_return (aio);
#ifdef DEBUG
	g_print ("numbytes %d for %d\n", numbytes, fd);
#endif
	} else {
		errno = result;
		_wapi_set_last_error_from_errno ();
		return FALSE;
	}

	if (byteswritten)
		*byteswritten = numbytes;

	return TRUE;
	}
#endif
}

static gboolean file_flush(gpointer handle)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(FALSE);
	}

	if(!(file_handle->fileaccess&GENERIC_WRITE) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_WRITE access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(FALSE);
	}

	ret=fsync(file_private_handle->fd);
	if (ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": write of handle %p fd %d error: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	
	return(TRUE);
}

static guint32 file_seek(gpointer handle, gint32 movedistance,
			 gint32 *highmovedistance, WapiSeekMethod method)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	off_t offset, newpos;
	int whence;
	guint32 ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(INVALID_SET_FILE_POINTER);
	}
	
	if(!(file_handle->fileaccess&GENERIC_READ) &&
	   !(file_handle->fileaccess&GENERIC_WRITE) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_READ or GENERIC_WRITE access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(INVALID_SET_FILE_POINTER);
	}

	switch(method) {
	case FILE_BEGIN:
		whence=SEEK_SET;
		break;
	case FILE_CURRENT:
		whence=SEEK_CUR;
		break;
	case FILE_END:
		whence=SEEK_END;
		break;
	default:
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": invalid seek type %d",
			  method);
#endif

		return(INVALID_SET_FILE_POINTER);
	}

#ifdef HAVE_LARGE_FILE_SUPPORT
	if(highmovedistance==NULL) {
		offset=movedistance;
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": setting offset to %lld (low %d)", offset,
			  movedistance);
#endif
	} else {
		offset=((gint64) *highmovedistance << 32) | movedistance;
		
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": setting offset to %lld 0x%llx (high %d 0x%x, low %d 0x%x)", offset, offset, *highmovedistance, *highmovedistance, movedistance, movedistance);
#endif
	}
#else
	offset=movedistance;
#endif

#ifdef DEBUG
#ifdef HAVE_LARGE_FILE_SUPPORT
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": moving handle %p fd %d by %lld bytes from %d", handle,
		  file_private_handle->fd, offset, whence);
#else
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": moving handle %p fd %d by %ld bytes from %d", handle,
		  file_private_handle->fd, offset, whence);
#endif
#endif

	newpos=lseek(file_private_handle->fd, offset, whence);
	if(newpos==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": lseek on handle %p fd %d returned error %s",
			  handle, file_private_handle->fd, strerror(errno));
#endif

		return(INVALID_SET_FILE_POINTER);
	}

#ifdef DEBUG
#ifdef HAVE_LARGE_FILE_SUPPORT
	g_message(G_GNUC_PRETTY_FUNCTION ": lseek returns %lld", newpos);
#else
	g_message(G_GNUC_PRETTY_FUNCTION ": lseek returns %ld", newpos);
#endif
#endif

#ifdef HAVE_LARGE_FILE_SUPPORT
	ret=newpos & 0xFFFFFFFF;
	if(highmovedistance!=NULL) {
		*highmovedistance=newpos>>32;
	}
#else
	ret=newpos;
	if(highmovedistance!=NULL) {
		/* Accurate, but potentially dodgy :-) */
		*highmovedistance=0;
	}
#endif

#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": move of handle %p fd %d returning %d/%d", handle,
		  file_private_handle->fd, ret,
		  highmovedistance==NULL?0:*highmovedistance);
#endif

	return(ret);
}

static gboolean file_setendoffile(gpointer handle)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	struct stat statbuf;
	off_t size, pos;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(FALSE);
	}
	
	if(!(file_handle->fileaccess&GENERIC_WRITE) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_WRITE access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(FALSE);
	}

	/* Find the current file position, and the file length.  If
	 * the file position is greater than the length, write to
	 * extend the file with a hole.  If the file position is less
	 * than the length, truncate the file.
	 */
	
	ret=fstat(file_private_handle->fd, &statbuf);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d fstat failed: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	size=statbuf.st_size;

	pos=lseek(file_private_handle->fd, (off_t)0, SEEK_CUR);
	if(pos==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d lseek failed: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	
	if(pos>size) {
		/* extend */
		ret=write(file_private_handle->fd, "", 1);
		if(ret==-1) {
#ifdef DEBUG
			g_message(G_GNUC_PRETTY_FUNCTION
				  ": handle %p fd %d extend write failed: %s",
				  handle, file_private_handle->fd,
				  strerror(errno));
#endif

			return(FALSE);
		}
	}

	/* always truncate, because the extend write() adds an extra
	 * byte to the end of the file
	 */
	ret=ftruncate(file_private_handle->fd, pos);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d ftruncate failed: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif
		
		return(FALSE);
	}
		
	return(TRUE);
}

static guint32 file_getfilesize(gpointer handle, guint32 *highsize)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	struct stat statbuf;
	guint32 size;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(INVALID_FILE_SIZE);
	}
	
	if(!(file_handle->fileaccess&GENERIC_READ) &&
	   !(file_handle->fileaccess&GENERIC_WRITE) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_READ or GENERIC_WRITE access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(INVALID_FILE_SIZE);
	}

	ret=fstat(file_private_handle->fd, &statbuf);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d fstat failed: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif

		return(INVALID_FILE_SIZE);
	}
	
#ifdef HAVE_LARGE_FILE_SUPPORT
	size=statbuf.st_size & 0xFFFFFFFF;
	if(highsize!=NULL) {
		*highsize=statbuf.st_size>>32;
	}
#else
	if(highsize!=NULL) {
		/* Accurate, but potentially dodgy :-) */
		*highsize=0;
	}
	size=statbuf.st_size;
#endif

#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Returning size %d/%d", size,
		  *highsize);
#endif
	
	return(size);
}

static gboolean file_getfiletime(gpointer handle, WapiFileTime *create_time,
				 WapiFileTime *last_access,
				 WapiFileTime *last_write)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	struct stat statbuf;
	guint64 create_ticks, access_ticks, write_ticks;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(FALSE);
	}
	
	if(!(file_handle->fileaccess&GENERIC_READ) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_READ access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(FALSE);
	}
	
	ret=fstat(file_private_handle->fd, &statbuf);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d fstat failed: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}

#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": atime: %ld ctime: %ld mtime: %ld",
		  statbuf.st_atime, statbuf.st_ctime,
		  statbuf.st_mtime);
#endif

	/* Try and guess a meaningful create time by using the older
	 * of atime or ctime
	 */
	/* The magic constant comes from msdn documentation
	 * "Converting a time_t Value to a File Time"
	 */
	if(statbuf.st_atime < statbuf.st_ctime) {
		create_ticks=((guint64)statbuf.st_atime*10000000)
			+ 116444736000000000ULL;
	} else {
		create_ticks=((guint64)statbuf.st_ctime*10000000)
			+ 116444736000000000ULL;
	}
	
	access_ticks=((guint64)statbuf.st_atime*10000000)+116444736000000000ULL;
	write_ticks=((guint64)statbuf.st_mtime*10000000)+116444736000000000ULL;
	
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": aticks: %llu cticks: %llu wticks: %llu",
			  access_ticks, create_ticks, write_ticks);
#endif

	if(create_time!=NULL) {
		create_time->dwLowDateTime = create_ticks & 0xFFFFFFFF;
		create_time->dwHighDateTime = create_ticks >> 32;
	}
	
	if(last_access!=NULL) {
		last_access->dwLowDateTime = access_ticks & 0xFFFFFFFF;
		last_access->dwHighDateTime = access_ticks >> 32;
	}
	
	if(last_write!=NULL) {
		last_write->dwLowDateTime = write_ticks & 0xFFFFFFFF;
		last_write->dwHighDateTime = write_ticks >> 32;
	}

	return(TRUE);
}

static gboolean file_setfiletime(gpointer handle,
				 const WapiFileTime *create_time G_GNUC_UNUSED,
				 const WapiFileTime *last_access,
				 const WapiFileTime *last_write)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	guchar *name;
	struct utimbuf utbuf;
	struct stat statbuf;
	guint64 access_ticks, write_ticks;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		return(FALSE);
	}
	
	if(!(file_handle->fileaccess&GENERIC_WRITE) &&
	   !(file_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_WRITE access: %u", handle, file_private_handle->fd, file_handle->fileaccess);
#endif

		return(FALSE);
	}

	if(file_handle->filename==0) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d unknown filename", handle,
			  file_private_handle->fd);
#endif

		return(FALSE);
	}
	
	/* Get the current times, so we can put the same times back in
	 * the event that one of the FileTime structs is NULL
	 */
	ret=fstat(file_private_handle->fd, &statbuf);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p fd %d fstat failed: %s", handle,
			  file_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}

	if(last_access!=NULL) {
		access_ticks=((guint64)last_access->dwHighDateTime << 32) +
			last_access->dwLowDateTime;
		utbuf.actime=(access_ticks - 116444736000000000ULL) / 10000000;
	} else {
		utbuf.actime=statbuf.st_atime;
	}

	if(last_write!=NULL) {
		write_ticks=((guint64)last_write->dwHighDateTime << 32) +
			last_write->dwLowDateTime;
		utbuf.modtime=(write_ticks - 116444736000000000ULL) / 10000000;
	} else {
		utbuf.modtime=statbuf.st_mtime;
	}

#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": setting handle %p access %ld write %ld", handle,
		  utbuf.actime, utbuf.modtime);
#endif

	name=_wapi_handle_scratch_lookup (file_handle->filename);

	ret=utime(name, &utbuf);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": handle %p [%s] fd %d utime failed: %s", handle,
			  name, file_private_handle->fd, strerror(errno));

#endif
		g_free (name);
		return(FALSE);
	}

	g_free (name);
	
	return(TRUE);
}

static void console_close_shared (gpointer handle)
{
	struct _WapiHandle_file *console_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_CONSOLE,
				(gpointer *)&console_handle, NULL);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up console handle %p", handle);
		return;
	}
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": closing console handle %p", handle);
#endif
	
	if(console_handle->filename!=0) {
		_wapi_handle_scratch_delete (console_handle->filename);
		console_handle->filename=0;
	}
	if(console_handle->security_attributes!=0) {
		_wapi_handle_scratch_delete (console_handle->security_attributes);
		console_handle->security_attributes=0;
	}
}

static void console_close_private (gpointer handle)
{
	struct _WapiHandlePrivate_file *console_private_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_CONSOLE, NULL,
				(gpointer *)&console_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up console handle %p", handle);
		return;
	}
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": closing console handle %p with fd %d", handle,
		  console_private_handle->fd);
#endif
	
	close(console_private_handle->fd);
}

static WapiFileType console_getfiletype(void)
{
	return(FILE_TYPE_CHAR);
}

static gboolean console_read(gpointer handle, gpointer buffer,
			     guint32 numbytes, guint32 *bytesread,
			     WapiOverlapped *overlapped G_GNUC_UNUSED)
{
	struct _WapiHandle_file *console_handle;
	struct _WapiHandlePrivate_file *console_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_CONSOLE,
				(gpointer *)&console_handle,
				(gpointer *)&console_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up console handle %p", handle);
		return(FALSE);
	}
	
	if(bytesread!=NULL) {
		*bytesread=0;
	}
	
	if(!(console_handle->fileaccess&GENERIC_READ) &&
	   !(console_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION": handle %p fd %d doesn't have GENERIC_READ access: %u", handle, console_private_handle->fd, console_handle->fileaccess);
#endif

		return(FALSE);
	}
	
	ret=read(console_private_handle->fd, buffer, numbytes);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": read of handle %p fd %d error: %s", handle,
			  console_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	
	if(bytesread!=NULL) {
		*bytesread=ret;
	}
	
	return(TRUE);
}

static gboolean console_write(gpointer handle, gconstpointer buffer,
			      guint32 numbytes, guint32 *byteswritten,
			      WapiOverlapped *overlapped G_GNUC_UNUSED)
{
	struct _WapiHandle_file *console_handle;
	struct _WapiHandlePrivate_file *console_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_CONSOLE,
				(gpointer *)&console_handle,
				(gpointer *)&console_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up console handle %p", handle);
		return(FALSE);
	}
	
	if(byteswritten!=NULL) {
		*byteswritten=0;
	}
	
	if(!(console_handle->fileaccess&GENERIC_WRITE) &&
	   !(console_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_WRITE access: %u", handle, console_private_handle->fd, console_handle->fileaccess);
#endif

		return(FALSE);
	}
	
	ret=write(console_private_handle->fd, buffer, numbytes);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": write of handle %p fd %d error: %s", handle,
			  console_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	if(byteswritten!=NULL) {
		*byteswritten=ret;
	}
	
	return(TRUE);
}

static void pipe_close_shared (gpointer handle)
{
	struct _WapiHandle_file *pipe_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_PIPE,
				(gpointer *)&pipe_handle, NULL);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up pipe handle %p", handle);
		return;
	}
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": closing pipe handle %p", handle);
#endif
	
	if(pipe_handle->filename!=0) {
		_wapi_handle_scratch_delete (pipe_handle->filename);
		pipe_handle->filename=0;
	}
	if(pipe_handle->security_attributes!=0) {
		_wapi_handle_scratch_delete (pipe_handle->security_attributes);
		pipe_handle->security_attributes=0;
	}
}

static void pipe_close_private (gpointer handle)
{
	struct _WapiHandlePrivate_file *pipe_private_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_PIPE, NULL,
				(gpointer *)&pipe_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up pipe handle %p", handle);
		return;
	}
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": closing pipe handle %p with fd %d", handle,
		  pipe_private_handle->fd);
#endif
	
	close(pipe_private_handle->fd);
}

static WapiFileType pipe_getfiletype(void)
{
	return(FILE_TYPE_PIPE);
}

static gboolean pipe_read (gpointer handle, gpointer buffer,
			   guint32 numbytes, guint32 *bytesread,
			   WapiOverlapped *overlapped G_GNUC_UNUSED)
{
	struct _WapiHandle_file *pipe_handle;
	struct _WapiHandlePrivate_file *pipe_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_PIPE,
				(gpointer *)&pipe_handle,
				(gpointer *)&pipe_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up pipe handle %p", handle);
		return(FALSE);
	}
	
	if(bytesread!=NULL) {
		*bytesread=0;
	}
	
	if(!(pipe_handle->fileaccess&GENERIC_READ) &&
	   !(pipe_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION": handle %p fd %d doesn't have GENERIC_READ access: %u", handle, pipe_private_handle->fd, pipe_handle->fileaccess);
#endif

		return(FALSE);
	}
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION
		   ": reading up to %d bytes from pipe %p (fd %d)", numbytes,
		   handle, pipe_private_handle->fd);
#endif

	ret=read(pipe_private_handle->fd, buffer, numbytes);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": read of handle %p fd %d error: %s", handle,
			  pipe_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": read %d bytes from pipe", ret);
#endif

	if(bytesread!=NULL) {
		*bytesread=ret;
	}
	
	return(TRUE);
}

static gboolean pipe_write(gpointer handle, gconstpointer buffer,
			   guint32 numbytes, guint32 *byteswritten,
			   WapiOverlapped *overlapped G_GNUC_UNUSED)
{
	struct _WapiHandle_file *pipe_handle;
	struct _WapiHandlePrivate_file *pipe_private_handle;
	gboolean ok;
	int ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_PIPE,
				(gpointer *)&pipe_handle,
				(gpointer *)&pipe_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up pipe handle %p", handle);
		return(FALSE);
	}
	
	if(byteswritten!=NULL) {
		*byteswritten=0;
	}
	
	if(!(pipe_handle->fileaccess&GENERIC_WRITE) &&
	   !(pipe_handle->fileaccess&GENERIC_ALL)) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": handle %p fd %d doesn't have GENERIC_WRITE access: %u", handle, pipe_private_handle->fd, pipe_handle->fileaccess);
#endif

		return(FALSE);
	}
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION
		   ": writing up to %d bytes to pipe %p (fd %d)", numbytes,
		   handle, pipe_private_handle->fd);
#endif
	
	ret=write(pipe_private_handle->fd, buffer, numbytes);
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": write of handle %p fd %d error: %s", handle,
			  pipe_private_handle->fd, strerror(errno));
#endif

		return(FALSE);
	}
	if(byteswritten!=NULL) {
		*byteswritten=ret;
	}
	
	return(TRUE);
}

static int convert_flags(guint32 fileaccess, guint32 createmode)
{
	int flags=0;
	
	switch(fileaccess) {
	case GENERIC_READ:
		flags=O_RDONLY;
		break;
	case GENERIC_WRITE:
		flags=O_WRONLY;
		break;
	case GENERIC_READ|GENERIC_WRITE:
		flags=O_RDWR;
		break;
	default:
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": Unknown access type 0x%x",
			  fileaccess);
#endif
		break;
	}

	switch(createmode) {
	case CREATE_NEW:
		flags|=O_CREAT|O_EXCL;
		break;
	case CREATE_ALWAYS:
		flags|=O_CREAT|O_TRUNC;
		break;
	case OPEN_EXISTING:
		break;
	case OPEN_ALWAYS:
		flags|=O_CREAT;
		break;
	case TRUNCATE_EXISTING:
		flags|=O_TRUNC;
		break;
	default:
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": Unknown create mode 0x%x",
			  createmode);
#endif
		break;
	}
	
	return(flags);
}

static guint32 convert_from_flags(int flags)
{
	guint32 fileaccess=0;
	
#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY|O_WRONLY|O_RDWR)
#endif

	if((flags & O_ACCMODE) == O_RDONLY) {
		fileaccess=GENERIC_READ;
	} else if ((flags & O_ACCMODE) == O_WRONLY) {
		fileaccess=GENERIC_WRITE;
	} else if ((flags & O_ACCMODE) == O_RDWR) {
		fileaccess=GENERIC_READ|GENERIC_WRITE;
	} else {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": Can't figure out flags 0x%x", flags);
#endif
	}

	/* Maybe sort out create mode too */

	return(fileaccess);
}

static mode_t convert_perms(guint32 sharemode)
{
	mode_t perms=0600;
	
	if(sharemode&FILE_SHARE_READ) {
		perms|=044;
	}
	if(sharemode&FILE_SHARE_WRITE) {
		perms|=022;
	}

	return(perms);
}


/**
 * CreateFile:
 * @name: a pointer to a NULL-terminated unicode string, that names
 * the file or other object to create.
 * @fileaccess: specifies the file access mode
 * @sharemode: whether the file should be shared.  This parameter is
 * currently ignored.
 * @security: Ignored for now.
 * @createmode: specifies whether to create a new file, whether to
 * overwrite an existing file, whether to truncate the file, etc.
 * @attrs: specifies file attributes and flags.  On win32 attributes
 * are characteristics of the file, not the handle, and are ignored
 * when an existing file is opened.  Flags give the library hints on
 * how to process a file to optimise performance.
 * @template: the handle of an open %GENERIC_READ file that specifies
 * attributes to apply to a newly created file, ignoring @attrs.
 * Normally this parameter is NULL.  This parameter is ignored when an
 * existing file is opened.
 *
 * Creates a new file handle.  This only applies to normal files:
 * pipes are handled by CreatePipe(), and console handles are created
 * with GetStdHandle().
 *
 * Return value: the new handle, or %INVALID_HANDLE_VALUE on error.
 */
gpointer CreateFile(const gunichar2 *name, guint32 fileaccess,
		    guint32 sharemode, WapiSecurityAttributes *security,
		    guint32 createmode, guint32 attrs,
		    gpointer template G_GNUC_UNUSED)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gpointer handle;
	gboolean ok;
	int flags=convert_flags(fileaccess, createmode);
	mode_t perms=convert_perms(sharemode);
	gchar *filename;
	int ret;
	int thr_ret;
	gpointer cf_ret = INVALID_HANDLE_VALUE;
	
	mono_once (&io_ops_once, io_ops_init);
	
	if(name==NULL) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": name is NULL");
#endif

		return(INVALID_HANDLE_VALUE);
	}

	filename=mono_unicode_to_external (name);
	if(filename==NULL) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": unicode conversion returned NULL");
#endif

		return(INVALID_HANDLE_VALUE);
	}
	
	ret=open(filename, flags, perms);
    
	/* If we were trying to open a directory with write permissions
	 * (e.g. O_WRONLY or O_RDWR), this call will fail with
	 * EISDIR. However, this is a bit bogus because calls to
	 * manipulate the directory (e.g. SetFileTime) will still work on
	 * the directory because they use other API calls
	 * (e.g. utime()). Hence, if we failed with the EISDIR error, try
	 * to open the directory again without write permission.
	 */
	if (ret == -1 && errno == EISDIR)
	{
		/* Try again but don't try to make it writable */
		ret=open(filename, flags  & ~(O_RDWR|O_WRONLY), perms);
	}
	
	if(ret==-1) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": Error opening file %s: %s",
			  filename, strerror(errno));
#endif
		_wapi_set_last_error_from_errno ();
		g_free (filename);

		return(INVALID_HANDLE_VALUE);
	}

	handle=_wapi_handle_new (WAPI_HANDLE_FILE);
	if(handle==_WAPI_HANDLE_INVALID) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error creating file handle");
		g_free (filename);

		return(INVALID_HANDLE_VALUE);
	}

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", handle);
		goto cleanup;
	}
	cf_ret = handle;

	file_private_handle->fd=ret;
	file_private_handle->assigned=TRUE;
	file_private_handle->async = ((attrs & FILE_FLAG_OVERLAPPED) != 0);
	file_handle->filename=_wapi_handle_scratch_store (filename,
							  strlen (filename));
	if(security!=NULL) {
		file_handle->security_attributes=_wapi_handle_scratch_store (
			security, sizeof(WapiSecurityAttributes));
	}
	
	file_handle->fileaccess=fileaccess;
	file_handle->sharemode=sharemode;
	file_handle->attrs=attrs;
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION
		  ": returning handle %p with fd %d", handle,
		  file_private_handle->fd);
#endif

cleanup:
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);
	
	g_free (filename);
	
	return(cf_ret);
}

/**
 * DeleteFile:
 * @name: a pointer to a NULL-terminated unicode string, that names
 * the file to be deleted.
 *
 * Deletes file @name.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean DeleteFile(const gunichar2 *name)
{
	gchar *filename;
	int ret;
	
	if(name==NULL) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": name is NULL");
#endif

		return(FALSE);
	}

	filename=mono_unicode_to_external(name);
	if(filename==NULL) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": unicode conversion returned NULL");
#endif

		return(FALSE);
	}
	
	ret=unlink(filename);
	
	g_free(filename);

	if(ret==0) {
		return(TRUE);
	}

	_wapi_set_last_error_from_errno ();
	return(FALSE);
}

/**
 * MoveFile:
 * @name: a pointer to a NULL-terminated unicode string, that names
 * the file to be moved.
 * @dest_name: a pointer to a NULL-terminated unicode string, that is the
 * new name for the file.
 *
 * Renames file @name to @dest_name
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean MoveFile (const gunichar2 *name, const gunichar2 *dest_name)
{
	gchar *utf8_name, *utf8_dest_name;
	int result;

	utf8_name = mono_unicode_to_external (name);
	if (utf8_name == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif
		
		return FALSE;
	}

	utf8_dest_name = mono_unicode_to_external (dest_name);
	if (utf8_dest_name == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif

		g_free (utf8_name);
		return FALSE;
	}

	result = rename (utf8_name, utf8_dest_name);
	g_free (utf8_name);
	g_free (utf8_dest_name);

	if (result != 0 && errno == EXDEV) {
		/* Try a copy to the new location, and delete the source */
		if (CopyFile (name, dest_name, TRUE)==FALSE) {
			return(FALSE);
		}
		
		return(DeleteFile (name));
	}

	if (result == 0) {
		return TRUE;
	}
	
	switch (errno) {
	case EEXIST:
		SetLastError (ERROR_ALREADY_EXISTS);
		break;
	
	default:
		_wapi_set_last_error_from_errno ();
		break;
	}

	return FALSE;
}

/**
 * CopyFile:
 * @name: a pointer to a NULL-terminated unicode string, that names
 * the file to be copied.
 * @dest_name: a pointer to a NULL-terminated unicode string, that is the
 * new name for the file.
 * @fail_if_exists: if TRUE and dest_name exists, the copy will fail.
 *
 * Copies file @name to @dest_name
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean CopyFile (const gunichar2 *name, const gunichar2 *dest_name,
		   gboolean fail_if_exists)
{
	gpointer src, dest;
	guint32 attrs;
	char buffer [32*1024];
	int remain, n;
	int fd_in, fd_out;
	struct stat st;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;

	attrs = GetFileAttributes (name);
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		SetLastError (ERROR_FILE_NOT_FOUND);
		return  FALSE;
	}
	
	src = CreateFile (name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			  NULL, OPEN_EXISTING, 0, NULL);
	if (src == INVALID_HANDLE_VALUE) {
		_wapi_set_last_error_from_errno ();
		return FALSE;
	}
	
	dest = CreateFile (dest_name, GENERIC_WRITE, 0, NULL,
			   fail_if_exists ? CREATE_NEW : CREATE_ALWAYS, attrs, NULL);
	if (dest == INVALID_HANDLE_VALUE) {
		_wapi_set_last_error_from_errno ();
		CloseHandle (src);
		return FALSE;
	}

	for (;;) {
		if (ReadFile (src, &buffer,sizeof (buffer), &remain, NULL) == 0) {
			_wapi_set_last_error_from_errno ();
#ifdef DEBUG
			g_message (G_GNUC_PRETTY_FUNCTION ": read failed.");
#endif
			CloseHandle (dest);
			CloseHandle (src);
			return FALSE;
		}

		if (remain == 0)
			break;

		while (remain > 0) {
			if (WriteFile (dest, &buffer, remain, &n, NULL) == 0) {
				_wapi_set_last_error_from_errno ();
#ifdef DEBUG
				g_message (G_GNUC_PRETTY_FUNCTION ": write failed.");
#endif
				CloseHandle (dest);
				CloseHandle (src);
				return FALSE;
			}

			remain -= n;
		}
	}

	ok=_wapi_lookup_handle (src, WAPI_HANDLE_FILE,
				NULL, (gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", src);

		goto done;
	}

	fd_in=file_private_handle->fd;
	fstat(fd_in, &st);
	
	ok=_wapi_lookup_handle (dest, WAPI_HANDLE_FILE,
				NULL, (gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up file handle %p", dest);

		goto done;
	}

	fd_out=file_private_handle->fd;
	fchmod(fd_out, st.st_mode);

done:
	CloseHandle (dest);
	CloseHandle (src);
	return TRUE;
}

static mono_once_t stdhandle_once=MONO_ONCE_INIT;
static gpointer stdin_handle=NULL;
static gpointer stdout_handle=NULL;
static gpointer stderr_handle=NULL;

static gpointer stdhandle_create (int fd, const guchar *name)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	gpointer handle, ret = NULL;
	int flags;
	int thr_ret;
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": creating standard handle type %s",
		  name);
#endif
	
	/* Check if fd is valid */
	flags=fcntl(fd, F_GETFL);
	if(flags==-1) {
		/* Invalid fd.  Not really much point checking for EBADF
		 * specifically
		 */
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": fcntl error on fd %d: %s",
			  fd, strerror(errno));
#endif

		return(INVALID_HANDLE_VALUE);
	}

	handle=_wapi_handle_new (WAPI_HANDLE_CONSOLE);
	if(handle==_WAPI_HANDLE_INVALID) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error creating file handle");
		return(NULL);
	}

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_CONSOLE,
				(gpointer *)&file_handle,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up console handle %p", handle);
		goto cleanup;
	}
	ret = handle;
	
	file_private_handle->fd=fd;
	file_private_handle->assigned=TRUE;
	file_handle->filename=_wapi_handle_scratch_store (name, strlen (name));
	/* some default security attributes might be needed */
	file_handle->security_attributes=0;
	file_handle->fileaccess=convert_from_flags(flags);
	file_handle->sharemode=0;
	file_handle->attrs=0;
	
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": returning handle %p with fd %d",
		  handle, file_private_handle->fd);
#endif

cleanup:
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);

	return(ret);
}

static void stdhandle_init (void)
{
	stdin_handle=stdhandle_create (0, "<stdin>");
	stdout_handle=stdhandle_create (1, "<stdout>");
	stderr_handle=stdhandle_create (2, "<stderr>");
}

/**
 * GetStdHandle:
 * @stdhandle: specifies the file descriptor
 *
 * Returns a handle for stdin, stdout, or stderr.  Always returns the
 * same handle for the same @stdhandle.
 *
 * Return value: the handle, or %INVALID_HANDLE_VALUE on error
 */

gpointer GetStdHandle(WapiStdHandle stdhandle)
{
	gpointer handle;
	
	mono_once (&io_ops_once, io_ops_init);
	mono_once (&stdhandle_once, stdhandle_init);
	
	switch(stdhandle) {
	case STD_INPUT_HANDLE:
		handle=stdin_handle;
		break;

	case STD_OUTPUT_HANDLE:
		handle=stdout_handle;
		break;

	case STD_ERROR_HANDLE:
		handle=stderr_handle;
		break;

	default:
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": unknown standard handle type");
#endif

		return(INVALID_HANDLE_VALUE);
	}

	/* Add a reference to this handle */
	_wapi_handle_ref (handle);
	
	return(handle);
}

/**
 * ReadFile:
 * @handle: The file handle to read from.  The handle must have
 * %GENERIC_READ access.
 * @buffer: The buffer to store read data in
 * @numbytes: The maximum number of bytes to read
 * @bytesread: The actual number of bytes read is stored here.  This
 * value can be zero if the handle is positioned at the end of the
 * file.
 * @overlapped: points to a required %WapiOverlapped structure if
 * @handle has the %FILE_FLAG_OVERLAPPED option set, should be NULL
 * otherwise.
 *
 * If @handle does not have the %FILE_FLAG_OVERLAPPED option set, this
 * function reads up to @numbytes bytes from the file from the current
 * file position, and stores them in @buffer.  If there are not enough
 * bytes left in the file, just the amount available will be read.
 * The actual number of bytes read is stored in @bytesread.

 * If @handle has the %FILE_FLAG_OVERLAPPED option set, the current
 * file position is ignored and the read position is taken from data
 * in the @overlapped structure.
 *
 * Return value: %TRUE if the read succeeds (even if no bytes were
 * read due to an attempt to read past the end of the file), %FALSE on
 * error.
 */
gboolean ReadFile(gpointer handle, gpointer buffer, guint32 numbytes,
		  guint32 *bytesread, WapiOverlapped *overlapped)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].readfile==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].readfile (handle, buffer, numbytes, bytesread,
				      overlapped));
}

/**
 * WriteFile:
 * @handle: The file handle to write to.  The handle must have
 * %GENERIC_WRITE access.
 * @buffer: The buffer to read data from.
 * @numbytes: The maximum number of bytes to write.
 * @byteswritten: The actual number of bytes written is stored here.
 * If the handle is positioned at the file end, the length of the file
 * is extended.  This parameter may be %NULL.
 * @overlapped: points to a required %WapiOverlapped structure if
 * @handle has the %FILE_FLAG_OVERLAPPED option set, should be NULL
 * otherwise.
 *
 * If @handle does not have the %FILE_FLAG_OVERLAPPED option set, this
 * function writes up to @numbytes bytes from @buffer to the file at
 * the current file position.  If @handle is positioned at the end of
 * the file, the file is extended.  The actual number of bytes written
 * is stored in @byteswritten.
 *
 * If @handle has the %FILE_FLAG_OVERLAPPED option set, the current
 * file position is ignored and the write position is taken from data
 * in the @overlapped structure.
 *
 * Return value: %TRUE if the write succeeds, %FALSE on error.
 */
gboolean WriteFile(gpointer handle, gconstpointer buffer, guint32 numbytes,
		   guint32 *byteswritten, WapiOverlapped *overlapped)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].writefile==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].writefile (handle, buffer, numbytes, byteswritten,
				       overlapped));
}

/**
 * FlushFileBuffers:
 * @handle: Handle to open file.  The handle must have
 * %GENERIC_WRITE access.
 *
 * Flushes buffers of the file and causes all unwritten data to
 * be written.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean FlushFileBuffers(gpointer handle)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].flushfile==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].flushfile (handle));
}

/**
 * SetEndOfFile:
 * @handle: The file handle to set.  The handle must have
 * %GENERIC_WRITE access.
 *
 * Moves the end-of-file position to the current position of the file
 * pointer.  This function is used to truncate or extend a file.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean SetEndOfFile(gpointer handle)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].setendoffile==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].setendoffile (handle));
}

/**
 * SetFilePointer:
 * @handle: The file handle to set.  The handle must have
 * %GENERIC_READ or %GENERIC_WRITE access.
 * @movedistance: Low 32 bits of a signed value that specifies the
 * number of bytes to move the file pointer.
 * @highmovedistance: Pointer to the high 32 bits of a signed value
 * that specifies the number of bytes to move the file pointer, or
 * %NULL.
 * @method: The starting point for the file pointer move.
 *
 * Sets the file pointer of an open file.
 *
 * The distance to move the file pointer is calculated from
 * @movedistance and @highmovedistance: If @highmovedistance is %NULL,
 * @movedistance is the 32-bit signed value; otherwise, @movedistance
 * is the low 32 bits and @highmovedistance a pointer to the high 32
 * bits of a 64 bit signed value.  A positive distance moves the file
 * pointer forward from the position specified by @method; a negative
 * distance moves the file pointer backward.
 *
 * If the library is compiled without large file support,
 * @highmovedistance is ignored and its value is set to zero on a
 * successful return.
 *
 * Return value: On success, the low 32 bits of the new file pointer.
 * If @highmovedistance is not %NULL, the high 32 bits of the new file
 * pointer are stored there.  On failure, %INVALID_SET_FILE_POINTER.
 */
guint32 SetFilePointer(gpointer handle, gint32 movedistance,
		       gint32 *highmovedistance, WapiSeekMethod method)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].seek==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].seek (handle, movedistance, highmovedistance,
				  method));
}

/**
 * GetFileType:
 * @handle: The file handle to test.
 *
 * Finds the type of file @handle.
 *
 * Return value: %FILE_TYPE_UNKNOWN - the type of the file @handle is
 * unknown.  %FILE_TYPE_DISK - @handle is a disk file.
 * %FILE_TYPE_CHAR - @handle is a character device, such as a console.
 * %FILE_TYPE_PIPE - @handle is a named or anonymous pipe.
 */
WapiFileType GetFileType(gpointer handle)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].getfiletype==NULL) {
		return(FILE_TYPE_UNKNOWN);
	}
	
	return(io_ops[type].getfiletype ());
}

/**
 * GetFileSize:
 * @handle: The file handle to query.  The handle must have
 * %GENERIC_READ or %GENERIC_WRITE access.
 * @highsize: If non-%NULL, the high 32 bits of the file size are
 * stored here.
 *
 * Retrieves the size of the file @handle.
 *
 * If the library is compiled without large file support, @highsize
 * has its value set to zero on a successful return.
 *
 * Return value: On success, the low 32 bits of the file size.  If
 * @highsize is non-%NULL then the high 32 bits of the file size are
 * stored here.  On failure %INVALID_FILE_SIZE is returned.
 */
guint32 GetFileSize(gpointer handle, guint32 *highsize)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].getfilesize==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].getfilesize (handle, highsize));
}

/**
 * GetFileTime:
 * @handle: The file handle to query.  The handle must have
 * %GENERIC_READ access.
 * @create_time: Points to a %WapiFileTime structure to receive the
 * number of ticks since the epoch that file was created.  May be
 * %NULL.
 * @last_access: Points to a %WapiFileTime structure to receive the
 * number of ticks since the epoch when file was last accessed.  May be
 * %NULL.
 * @last_write: Points to a %WapiFileTime structure to receive the
 * number of ticks since the epoch when file was last written to.  May
 * be %NULL.
 *
 * Finds the number of ticks since the epoch that the file referenced
 * by @handle was created, last accessed and last modified.  A tick is
 * a 100 nanosecond interval.  The epoch is Midnight, January 1 1601
 * GMT.
 *
 * Create time isn't recorded on POSIX file systems or reported by
 * stat(2), so that time is guessed by returning the oldest of the
 * other times.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean GetFileTime(gpointer handle, WapiFileTime *create_time,
		     WapiFileTime *last_access, WapiFileTime *last_write)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].getfiletime==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].getfiletime (handle, create_time, last_access,
					 last_write));
}

/**
 * SetFileTime:
 * @handle: The file handle to set.  The handle must have
 * %GENERIC_WRITE access.
 * @create_time: Points to a %WapiFileTime structure that contains the
 * number of ticks since the epoch that the file was created.  May be
 * %NULL.
 * @last_access: Points to a %WapiFileTime structure that contains the
 * number of ticks since the epoch when the file was last accessed.
 * May be %NULL.
 * @last_write: Points to a %WapiFileTime structure that contains the
 * number of ticks since the epoch when the file was last written to.
 * May be %NULL.
 *
 * Sets the number of ticks since the epoch that the file referenced
 * by @handle was created, last accessed or last modified.  A tick is
 * a 100 nanosecond interval.  The epoch is Midnight, January 1 1601
 * GMT.
 *
 * Create time isn't recorded on POSIX file systems, and is ignored.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean SetFileTime(gpointer handle, const WapiFileTime *create_time,
		     const WapiFileTime *last_access,
		     const WapiFileTime *last_write)
{
	WapiHandleType type=_wapi_handle_type (handle);
	
	if(io_ops[type].setfiletime==NULL) {
		return(FALSE);
	}
	
	return(io_ops[type].setfiletime (handle, create_time, last_access,
					 last_write));
}

/* A tick is a 100-nanosecond interval.  File time epoch is Midnight,
 * January 1 1601 GMT
 */

#define TICKS_PER_MILLISECOND 10000L
#define TICKS_PER_SECOND 10000000L
#define TICKS_PER_MINUTE 600000000L
#define TICKS_PER_HOUR 36000000000LL
#define TICKS_PER_DAY 864000000000LL

#define isleap(y) ((y) % 4 == 0 && ((y) % 100 != 0 || (y) % 400 == 0))

static const guint16 mon_yday[2][13]={
	{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
	{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

/**
 * FileTimeToSystemTime:
 * @file_time: Points to a %WapiFileTime structure that contains the
 * number of ticks to convert.
 * @system_time: Points to a %WapiSystemTime structure to receive the
 * broken-out time.
 *
 * Converts a tick count into broken-out time values.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean FileTimeToSystemTime(const WapiFileTime *file_time,
			      WapiSystemTime *system_time)
{
	gint64 file_ticks, totaldays, rem, y;
	const guint16 *ip;
	
	if(system_time==NULL) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": system_time NULL");
#endif

		return(FALSE);
	}
	
	file_ticks=((gint64)file_time->dwHighDateTime << 32) +
		file_time->dwLowDateTime;
	
	/* Really compares if file_ticks>=0x8000000000000000
	 * (LLONG_MAX+1) but we're working with a signed value for the
	 * year and day calculation to work later
	 */
	if(file_ticks<0) {
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": file_time too big");
#endif

		return(FALSE);
	}

	totaldays=(file_ticks / TICKS_PER_DAY);
	rem = file_ticks % TICKS_PER_DAY;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": totaldays: %lld rem: %lld",
		  totaldays, rem);
#endif

	system_time->wHour=rem/TICKS_PER_HOUR;
	rem %= TICKS_PER_HOUR;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Hour: %d rem: %lld",
		  system_time->wHour, rem);
#endif
	
	system_time->wMinute = rem / TICKS_PER_MINUTE;
	rem %= TICKS_PER_MINUTE;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Minute: %d rem: %lld",
		  system_time->wMinute, rem);
#endif
	
	system_time->wSecond = rem / TICKS_PER_SECOND;
	rem %= TICKS_PER_SECOND;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Second: %d rem: %lld",
		  system_time->wSecond, rem);
#endif
	
	system_time->wMilliseconds = rem / TICKS_PER_MILLISECOND;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Milliseconds: %d",
		  system_time->wMilliseconds);
#endif

	/* January 1, 1601 was a Monday, according to Emacs calendar */
	system_time->wDayOfWeek = ((1 + totaldays) % 7) + 1;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Day of week: %d",
		  system_time->wDayOfWeek);
#endif
	
	/* This algorithm to find year and month given days from epoch
	 * from glibc
	 */
	y=1601;
	
#define DIV(a, b) ((a) / (b) - ((a) % (b) < 0))
#define LEAPS_THRU_END_OF(y) (DIV(y, 4) - DIV (y, 100) + DIV (y, 400))

	while(totaldays < 0 || totaldays >= (isleap(y)?366:365)) {
		/* Guess a corrected year, assuming 365 days per year */
		gint64 yg = y + totaldays / 365 - (totaldays % 365 < 0);
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": totaldays: %lld yg: %lld y: %lld", totaldays, yg,
			  y);
		g_message(G_GNUC_PRETTY_FUNCTION
			  ": LEAPS(yg): %lld LEAPS(y): %lld",
			  LEAPS_THRU_END_OF(yg-1), LEAPS_THRU_END_OF(y-1));
#endif
		
		/* Adjust days and y to match the guessed year. */
		totaldays -= ((yg - y) * 365
			      + LEAPS_THRU_END_OF (yg - 1)
			      - LEAPS_THRU_END_OF (y - 1));
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": totaldays: %lld",
			  totaldays);
#endif
		y = yg;
#ifdef DEBUG
		g_message(G_GNUC_PRETTY_FUNCTION ": y: %lld", y);
#endif
	}
	
	system_time->wYear = y;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Year: %d", system_time->wYear);
#endif

	ip = mon_yday[isleap(y)];
	
	for(y=11; totaldays < ip[y]; --y) {
		continue;
	}
	totaldays-=ip[y];
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": totaldays: %lld", totaldays);
#endif
	
	system_time->wMonth = y + 1;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Month: %d", system_time->wMonth);
#endif

	system_time->wDay = totaldays + 1;
#ifdef DEBUG
	g_message(G_GNUC_PRETTY_FUNCTION ": Day: %d", system_time->wDay);
#endif
	
	return(TRUE);
}

static gint
file_compare (gconstpointer a, gconstpointer b)
{
	gchar *astr = *(gchar **) a;
	gchar *bstr = *(gchar **) b;

	return strcmp (astr, bstr);
}

/* scandir using glib */
static gint
mono_io_scandir (const gchar *dirname, const gchar *pattern, gchar ***namelist)
{
	GError *error = NULL;
	GDir *dir;
	GPtrArray *names;
	const gchar *name;
	gint result;
	GPatternSpec *patspec;

	dir = g_dir_open (dirname, 0, &error);
	if (dir == NULL) {
		errno = error->code;
		g_error_free (error);
		return -1;
	}

	patspec = g_pattern_spec_new (pattern);
	names = g_ptr_array_new ();
	while ((name = g_dir_read_name (dir)) != NULL) {
		if (g_pattern_match_string (patspec, name))
			g_ptr_array_add (names, g_strdup (name));
	}
	
	g_pattern_spec_free (patspec);
	g_dir_close (dir);
	result = names->len;
	if (result > 0) {
		g_ptr_array_sort (names, file_compare);
		g_ptr_array_set_size (names, result + 1);

		*namelist = (gchar **) g_ptr_array_free (names, FALSE);
	} else {
		g_ptr_array_free (names, TRUE);
	}

	return result;
}

gpointer FindFirstFile (const gunichar2 *pattern, WapiFindData *find_data)
{
	struct _WapiHandlePrivate_find *find_handle;
	gpointer handle, find_ret = INVALID_HANDLE_VALUE;
	gboolean ok;
	gchar *utf8_pattern = NULL, *dir_part, *entry_part;
	int result;
	int thr_ret;
	gboolean unref = FALSE;
	
	if (pattern == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": pattern is NULL");
#endif

		SetLastError (ERROR_PATH_NOT_FOUND);
		return(INVALID_HANDLE_VALUE);
	}

	utf8_pattern = mono_unicode_to_external (pattern);
	if (utf8_pattern == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif
		
		SetLastError (ERROR_INVALID_NAME);
		return(INVALID_HANDLE_VALUE);
	}

#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": looking for [%s]",
		utf8_pattern);
#endif
	
	/* Figure out which bit of the pattern is the directory */
	dir_part=g_path_get_dirname (utf8_pattern);
	entry_part=g_path_get_basename (utf8_pattern);

#if 0
	/* Don't do this check for now, it breaks if directories
	 * really do have metachars in their names (see bug 58116).
	 * FIXME: Figure out a better solution to keep some checks...
	 */
	if (strchr (dir_part, '*') || strchr (dir_part, '?')) {
		SetLastError (ERROR_INVALID_NAME);
		g_free (dir_part);
		g_free (entry_part);
		g_free (utf8_pattern);
		return(INVALID_HANDLE_VALUE);
	}
#endif
	
	handle=_wapi_handle_new (WAPI_HANDLE_FIND);
	if(handle==_WAPI_HANDLE_INVALID) {
		g_warning (G_GNUC_PRETTY_FUNCTION ": error creating find handle");
		g_free (dir_part);
		g_free (entry_part);
		g_free (utf8_pattern);
		
		return(INVALID_HANDLE_VALUE);
	}

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FIND, NULL,
				(gpointer *)&find_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up find handle %p", handle);
		g_free (dir_part);
		dir_part = NULL;
		g_free (entry_part);
		entry_part = NULL;
		g_free (utf8_pattern);
		utf8_pattern = NULL;
		goto cleanup;
	}

	/* The pattern can specify a directory or a set of files.
	 *
	 * The pattern can have wildcard characters ? and *, but only
	 * in the section after the last directory delimiter.  (Return
	 * ERROR_INVALID_NAME if there are wildcards in earlier path
	 * sections.)  "*" has the usual 0-or-more chars meaning.  "?" 
	 * means "match one character", "??" seems to mean "match one
	 * or two characters", "???" seems to mean "match one, two or
	 * three characters", etc.  Windows will also try and match
	 * the mangled "short name" of files, so 8 character patterns
	 * with wildcards will show some surprising results.
	 *
	 * All the written documentation I can find says that '?' 
	 * should only match one character, and doesn't mention '??',
	 * '???' etc.  I'm going to assume that the strict behaviour
	 * (ie '???' means three and only three characters) is the
	 * correct one, because that lets me use fnmatch(3) rather
	 * than mess around with regexes.
	 */

	result = mono_io_scandir (dir_part, entry_part, &find_handle->namelist);
	g_free (utf8_pattern);
	g_free (entry_part);
	
	if (result < 0) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": scandir error: %s", g_strerror (errno));
#endif
		SetLastError (ERROR_INVALID_PARAMETER);
		g_free (dir_part);
		unref = TRUE;
		goto cleanup;
	}
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": Got %d matches", result);
#endif

	find_handle->dir_part = dir_part;
	find_handle->num = result;
	find_handle->count = 0;

	find_ret = handle;

cleanup:
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);

	/* FindNextFile has to be called after unlocking the handle,
	 * because it wants to lock the handle itself
	 */
	if (find_ret != INVALID_HANDLE_VALUE &&
	    !FindNextFile (handle, find_data)) {
		FindClose (handle);
		SetLastError (ERROR_NO_MORE_FILES);
		find_ret = INVALID_HANDLE_VALUE;
	}

	/* Must not call _wapi_handle_unref() with the handle already
	 * locked
	 */
	if (unref) {
		_wapi_handle_unref (handle);
	}
	
	return (find_ret);
}

gboolean FindNextFile (gpointer handle, WapiFindData *find_data)
{
	struct _WapiHandlePrivate_find *find_handle;
	gboolean ok;
	struct stat buf;
	gchar *filename;
	gchar *utf8_filename, *utf8_basename;
	gunichar2 *utf16_basename;
	time_t create_time;
	glong bytes;
	int thr_ret;
	gboolean ret = FALSE;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FIND, NULL,
				(gpointer *)&find_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up find handle %p", handle);
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
retry:
	if (find_handle->count >= find_handle->num) {
		SetLastError (ERROR_NO_MORE_FILES);
		goto cleanup;
	}

	/* stat next match */

	filename = g_build_filename (find_handle->dir_part, find_handle->namelist[find_handle->count ++], NULL);
	if (lstat (filename, &buf) != 0) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": stat failed: %s", filename);
#endif

		g_free (filename);
		goto retry;
	}

	/* Check for dangling symlinks, and ignore them (principle of
	 * least surprise, avoiding confusion where we report the file
	 * exists, but when someone tries to open it we would report
	 * it isn't there.)
	 */
	if(S_ISLNK (buf.st_mode)) {
		if(stat (filename, &buf) != 0) {
			g_free (filename);
			goto retry;
		}
	}
	
	utf8_filename=mono_utf8_from_external (filename);
	if(utf8_filename==NULL) {
		/* We couldn't turn this filename into utf8 (eg the
		 * encoding of the name wasn't convertible), so just
		 * ignore it.
		 */
		g_free (filename);
		goto retry;
	}
	g_free (filename);
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": Found [%s]", utf8_filename);
#endif
	
	/* fill data block */

	if (buf.st_mtime < buf.st_ctime)
		create_time = buf.st_mtime;
	else
		create_time = buf.st_ctime;
	
	find_data->dwFileAttributes = _wapi_stat_to_file_attributes (&buf);

	_wapi_time_t_to_filetime (create_time, &find_data->ftCreationTime);
	_wapi_time_t_to_filetime (buf.st_atime, &find_data->ftLastAccessTime);
	_wapi_time_t_to_filetime (buf.st_mtime, &find_data->ftLastWriteTime);

	if (find_data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		find_data->nFileSizeHigh = 0;
		find_data->nFileSizeLow = 0;
	}
	else {
		find_data->nFileSizeHigh = buf.st_size >> 32;
		find_data->nFileSizeLow = buf.st_size & 0xFFFFFFFF;
	}

	find_data->dwReserved0 = 0;
	find_data->dwReserved1 = 0;

	utf8_basename = g_path_get_basename (utf8_filename);
	utf16_basename = g_utf8_to_utf16 (utf8_basename, -1, NULL, &bytes,
					  NULL);
	if(utf16_basename==NULL) {
		g_free (utf8_basename);
		g_free (utf8_filename);
		goto retry;
	}
	ret = TRUE;
	
	/* utf16 is 2 * utf8 */
	bytes *= 2;

	memset (find_data->cFileName, '\0', (MAX_PATH*2));

	/* Truncating a utf16 string like this might leave the last
	 * char incomplete
	 */
	memcpy (find_data->cFileName, utf16_basename,
		bytes<(MAX_PATH*2)-2?bytes:(MAX_PATH*2)-2);

	find_data->cAlternateFileName [0] = 0;	/* not used */

	g_free (utf8_basename);
	g_free (utf8_filename);
	g_free (utf16_basename);

cleanup:
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);
	
	return(ret);
}

/**
 * FindClose:
 * @wapi_handle: the find handle to close.
 *
 * Closes find handle @wapi_handle
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean FindClose (gpointer handle)
{
	struct _WapiHandlePrivate_find *find_handle;
	gboolean ok;
	int thr_ret;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FIND, NULL,
				(gpointer *)&find_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error looking up find handle %p", handle);
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	g_strfreev (find_handle->namelist);
	g_free (find_handle->dir_part);

	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);
	
	_wapi_handle_unref (handle);
	
	return(TRUE);
}

/**
 * CreateDirectory:
 * @name: a pointer to a NULL-terminated unicode string, that names
 * the directory to be created.
 * @security: ignored for now
 *
 * Creates directory @name
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean CreateDirectory (const gunichar2 *name, WapiSecurityAttributes *security)
{
	gchar *utf8_name;
	int result;
	struct stat buf;
	guint32 attrs;
	
	utf8_name = mono_unicode_to_external (name);
	if (utf8_name == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif
	
		return FALSE;
	}

	result = mkdir (utf8_name, 0777);

	if (result == 0) {
		g_free (utf8_name);
		return TRUE;
	}

	if (errno == EEXIST) {
		result = stat (utf8_name, &buf);
		if (result == -1) {
			_wapi_set_last_error_from_errno ();
			g_free (utf8_name);
			return FALSE;
		}

		g_free (utf8_name);
		attrs = _wapi_stat_to_file_attributes (&buf);
		if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY)
			return TRUE;

		errno = EEXIST;
		_wapi_set_last_error_from_errno ();
		return FALSE;
	}

	_wapi_set_last_error_from_errno ();
	g_free (utf8_name);
	return FALSE;
}

/**
 * RemoveDirectory:
 * @name: a pointer to a NULL-terminated unicode string, that names
 * the directory to be removed.
 *
 * Removes directory @name
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean RemoveDirectory (const gunichar2 *name)
{
	gchar *utf8_name;
	int result;

	utf8_name = mono_unicode_to_external (name);
	if (utf8_name == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif
		
		return FALSE;
	}

	result = rmdir (utf8_name);
	g_free (utf8_name);

	if (result == 0)
		return TRUE;
	
	_wapi_set_last_error_from_errno ();
	return FALSE;
}

/**
 * GetFileAttributes:
 * @name: a pointer to a NULL-terminated unicode filename.
 *
 * Gets the attributes for @name;
 *
 * Return value: %INVALID_FILE_ATTRIBUTES on failure
 */
guint32 GetFileAttributes (const gunichar2 *name)
{
	gchar *utf8_name;
	struct stat buf;
	int result;
	
	utf8_name = mono_unicode_to_external (name);
	if (utf8_name == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif

		SetLastError (ERROR_INVALID_PARAMETER);
		return (INVALID_FILE_ATTRIBUTES);
	}

	result = stat (utf8_name, &buf);

	if (result != 0) {
		_wapi_set_last_error_from_errno ();
		g_free (utf8_name);
		return (INVALID_FILE_ATTRIBUTES);
	}
	
	g_free (utf8_name);
	return _wapi_stat_to_file_attributes (&buf);
}

/**
 * GetFileAttributesEx:
 * @name: a pointer to a NULL-terminated unicode filename.
 * @level: must be GetFileExInfoStandard
 * @info: pointer to a WapiFileAttributesData structure
 *
 * Gets attributes, size and filetimes for @name;
 *
 * Return value: %TRUE on success, %FALSE on failure
 */
gboolean GetFileAttributesEx (const gunichar2 *name, WapiGetFileExInfoLevels level, gpointer info)
{
	gchar *utf8_name;
	WapiFileAttributesData *data;

	struct stat buf;
	time_t create_time;
	int result;
	
	if (level != GetFileExInfoStandard) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": info level %d not supported.", level);
#endif

		return FALSE;
	}

	utf8_name = mono_unicode_to_external (name);
	if (utf8_name == NULL) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": unicode conversion returned NULL");
#endif

		SetLastError (ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	result = stat (utf8_name, &buf);
	g_free (utf8_name);

	if (result != 0) {
		SetLastError (ERROR_FILE_NOT_FOUND);
		return FALSE;
	}

	/* fill data block */

	data = (WapiFileAttributesData *)info;

	if (buf.st_mtime < buf.st_ctime)
		create_time = buf.st_mtime;
	else
		create_time = buf.st_ctime;
	
	data->dwFileAttributes = _wapi_stat_to_file_attributes (&buf);

	_wapi_time_t_to_filetime (create_time, &data->ftCreationTime);
	_wapi_time_t_to_filetime (buf.st_atime, &data->ftLastAccessTime);
	_wapi_time_t_to_filetime (buf.st_mtime, &data->ftLastWriteTime);

	if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		data->nFileSizeHigh = 0;
		data->nFileSizeLow = 0;
	}
	else {
		data->nFileSizeHigh = buf.st_size >> 32;
		data->nFileSizeLow = buf.st_size & 0xFFFFFFFF;
	}

	return TRUE;
}

/**
 * SetFileAttributes
 * @name: name of file
 * @attrs: attributes to set
 *
 * Changes the attributes on a named file.
 *
 * Return value: %TRUE on success, %FALSE on failure.
 */
extern gboolean SetFileAttributes (const gunichar2 *name, guint32 attrs)
{
	/* FIXME: think of something clever to do on unix */
	gchar *utf8_name;
	struct stat buf;
	int result;

	/*
	 * Currently we only handle one *internal* case, with a value that is
	 * not standard: 0x80000000, which means `set executable bit'
	 */

	utf8_name = mono_unicode_to_external (name);
	result = stat (utf8_name, &buf);
	if (result != 0) {
		g_free (utf8_name);
		SetLastError (ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	
	/* Contrary to the documentation, ms allows NORMAL to be
	 * specified along with other attributes, so dont bother to
	 * catch that case here.
	 */
	if (attrs & FILE_ATTRIBUTE_READONLY) {
		result = chmod (utf8_name, buf.st_mode & ~(S_IWRITE | S_IWOTH | S_IWGRP));
	} else {
		result = chmod (utf8_name, buf.st_mode | S_IWRITE);
	}

	/* Ignore the other attributes for now */

	if (attrs & 0x80000000){
		result = chmod (utf8_name, buf.st_mode | S_IEXEC | S_IXOTH | S_IXGRP);
	}
	/* Don't bother to reset executable (might need to change this
	 * policy)
	 */
	
	g_free (utf8_name);

	return(TRUE);
}

/**
 * GetCurrentDirectory
 * @length: size of the buffer
 * @buffer: pointer to buffer that recieves path
 *
 * Retrieves the current directory for the current process.
 *
 * Return value: number of characters in buffer on success, zero on failure
 */
extern guint32 GetCurrentDirectory (guint32 length, gunichar2 *buffer)
{
	gchar *path;
	gunichar2 *utf16_path;
	glong count;
	gsize bytes;
	
	path = g_get_current_dir ();
	if (path == NULL)
		return 0;

	utf16_path=mono_unicode_from_external (path, &bytes);
	
	/* if buffer too small, return number of characters required.
	 * this is plain dumb.
	 */
	
	count = (bytes/2)+1;
	if (count > length) {
		g_free(path);
		g_free (utf16_path);
		
		return (count);
	}

	/* Add the terminator */
	memset (buffer, '\0', bytes+2);
	memcpy (buffer, utf16_path, bytes);
	
	g_free (utf16_path);
	g_free (path);

	return count;
}

/**
 * SetCurrentDirectory
 * @path: path to new directory
 *
 * Changes the directory path for the current process.
 *
 * Return value: %TRUE on success, %FALSE on failure.
 */
extern gboolean SetCurrentDirectory (const gunichar2 *path)
{
	gchar *utf8_path;
	gboolean result;

	utf8_path = mono_unicode_to_external (path);
	if (chdir (utf8_path) != 0) {
		_wapi_set_last_error_from_errno ();
		result = FALSE;
	}
	else
		result = TRUE;

	g_free (utf8_path);
	return result;
}

int _wapi_file_handle_to_fd (gpointer handle)
{
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": looking up fd for %p", handle);
#endif

	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_CONSOLE, NULL,
				(gpointer *)&file_private_handle);
	if(ok==FALSE) {
		ok=_wapi_lookup_handle (handle, WAPI_HANDLE_FILE, NULL,
					(gpointer *)&file_private_handle);
		if(ok==FALSE) {
			ok=_wapi_lookup_handle (handle, WAPI_HANDLE_PIPE, NULL,
						(gpointer *)&file_private_handle);
			if(ok==FALSE) {
#ifdef DEBUG
				g_message (G_GNUC_PRETTY_FUNCTION
					   ": returning -1");
#endif
				return(-1);
			}
		}
	}
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": returning %d",
		   file_private_handle->fd);
#endif
	
	return(file_private_handle->fd);
}

gboolean CreatePipe (gpointer *readpipe, gpointer *writepipe,
		     WapiSecurityAttributes *security G_GNUC_UNUSED, guint32 size)
{
	struct _WapiHandle_file *pipe_read_handle;
	struct _WapiHandle_file *pipe_write_handle;
	struct _WapiHandlePrivate_file *pipe_read_private_handle;
	struct _WapiHandlePrivate_file *pipe_write_private_handle;
	gpointer read_handle;
	gpointer write_handle;
	gboolean ok;
	int filedes[2];
	int ret;
	int thr_ret;
	gboolean unref_read = FALSE, unref_write = FALSE;
	gboolean cp_ret = FALSE;
	
	mono_once (&io_ops_once, io_ops_init);
	
#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION ": Creating pipe");
#endif

	ret=pipe (filedes);
	if(ret==-1) {
#ifdef DEBUG
		g_message (G_GNUC_PRETTY_FUNCTION ": Error creating pipe: %s",
			   strerror (errno));
#endif
		
		_wapi_set_last_error_from_errno ();
		return(FALSE);
	}
	
	/* filedes[0] is open for reading, filedes[1] for writing */

	read_handle=_wapi_handle_new (WAPI_HANDLE_PIPE);
	if(read_handle==_WAPI_HANDLE_INVALID) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error creating pipe read handle");
		close (filedes[0]);
		close (filedes[1]);
		return(FALSE);
	}
	
	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      read_handle);
	thr_ret = _wapi_handle_lock_handle (read_handle);
	g_assert (thr_ret == 0);

	ok=_wapi_lookup_handle (read_handle, WAPI_HANDLE_PIPE,
				(gpointer *)&pipe_read_handle,
				(gpointer *)&pipe_read_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION ": error looking up pipe handle %p", read_handle);
		close (filedes[0]);
		close (filedes[1]);
		goto cleanup;
	}
	
	write_handle=_wapi_handle_new (WAPI_HANDLE_PIPE);
	if(write_handle==_WAPI_HANDLE_INVALID) {
		g_warning (G_GNUC_PRETTY_FUNCTION
			   ": error creating pipe write handle");
		unref_read = TRUE;
		
		close (filedes[0]);
		close (filedes[1]);
		goto cleanup;
	}
	
	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      write_handle);
	thr_ret = _wapi_handle_lock_handle (write_handle);
	g_assert (thr_ret == 0);

	ok=_wapi_lookup_handle (write_handle, WAPI_HANDLE_PIPE,
				(gpointer *)&pipe_write_handle,
				(gpointer *)&pipe_write_private_handle);
	if(ok==FALSE) {
		g_warning (G_GNUC_PRETTY_FUNCTION ": error looking up pipe handle %p", read_handle);
		unref_read = TRUE;
		unref_write = TRUE;
		
		close (filedes[0]);
		close (filedes[1]);
		goto write_cleanup;
	}
	cp_ret = TRUE;
	
	pipe_read_private_handle->fd=filedes[0];
	pipe_read_private_handle->assigned=TRUE;
	pipe_read_handle->fileaccess=GENERIC_READ;
	
	*readpipe=read_handle;

	pipe_write_private_handle->fd=filedes[1];
	pipe_write_private_handle->assigned=TRUE;
	pipe_write_handle->fileaccess=GENERIC_WRITE;
	
	*writepipe=write_handle;

#ifdef DEBUG
	g_message (G_GNUC_PRETTY_FUNCTION
		   ": Returning pipe: read handle %p, write handle %p",
		   read_handle, write_handle);
#endif

write_cleanup:
	thr_ret =_wapi_handle_unlock_handle (write_handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);

cleanup:
	thr_ret =_wapi_handle_unlock_handle (read_handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);

	/* Must not call _wapi_handle_unref() with the handle already
	 * locked
	 */
	if (unref_read) {
		_wapi_handle_unref (read_handle);
	}
	if (unref_write) {
		_wapi_handle_unref (write_handle);
	}
	
	return(cp_ret);
}

guint32 GetTempPath (guint32 len, gunichar2 *buf)
{
	gchar *tmpdir=g_strdup (g_get_tmp_dir ());
	gunichar2 *tmpdir16=NULL;
	glong dirlen;
	gsize bytes;
	guint32 ret;
	
	if(tmpdir[strlen (tmpdir)]!='/') {
		g_free (tmpdir);
		tmpdir=g_strdup_printf ("%s/", g_get_tmp_dir ());
	}
	
	tmpdir16=mono_unicode_from_external (tmpdir, &bytes);
	if(tmpdir16==NULL) {
		g_free (tmpdir);
		return(0);
	} else {
		dirlen=(bytes/2);
		
		if(dirlen+1>len) {
#ifdef DEBUG
			g_message (G_GNUC_PRETTY_FUNCTION
				   ": Size %d smaller than needed (%ld)", len,
				   dirlen+1);
#endif
		
			ret=dirlen+1;
		} else {
			/* Add the terminator */
			memset (buf, '\0', bytes+2);
			memcpy (buf, tmpdir16, bytes);
		
			ret=dirlen;
		}
	}

	if(tmpdir16!=NULL) {
		g_free (tmpdir16);
	}
	g_free (tmpdir);
	
	return(ret);
}

gboolean
_wapi_io_add_callback (gpointer handle,
		       WapiOverlappedCB callback,
		       guint64 flags G_GNUC_UNUSED)
{
	struct _WapiHandle_file *file_handle;
	struct _WapiHandlePrivate_file *file_private_handle;
	gboolean ok;
	int thr_ret;
	gboolean ret = FALSE;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_FILE,
				  (gpointer *) &file_handle,
				  (gpointer *) &file_private_handle);

	if (ok == FALSE) {
		ok = _wapi_lookup_handle (handle, WAPI_HANDLE_PIPE,
					  (gpointer *) &file_handle,
					  (gpointer *) &file_private_handle);

	}

	if (ok == FALSE || file_private_handle->async == FALSE) {
		SetLastError (ERROR_INVALID_HANDLE);
		return FALSE;
	}

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	if (file_private_handle->callback != NULL) {
		SetLastError (ERROR_INVALID_PARAMETER);
		goto cleanup;
	}
	ret = TRUE;
	
	file_private_handle->callback = callback;

cleanup:
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);

	return(ret);
}

gint32
GetLogicalDriveStrings (guint32 len, gunichar2 *buf)
{
	FILE *fp;
	gunichar2 *ptr, *dir;
	glong length, total = 0;
	gchar buffer [512];
	gchar **splitted;

	memset (buf, 0, sizeof (gunichar2) * (len + 1)); 
	buf [0] = '/';
	buf [1] = 0;
	buf [2] = 0;

	/* Sigh, mntent and friends don't work well.
	 * It stops on the first line that doesn't begin with a '/'.
	 * (linux 2.6.5, libc 2.3.2.ds1-12) - Gonz */
	fp = fopen ("/etc/mtab", "rt");
	if (fp == NULL) {
		fp = fopen ("/etc/mnttab", "rt");
		if (fp == NULL)
			return 1;
	}

	ptr = buf;
	while (fgets (buffer, 512, fp) != NULL) {
		if (*buffer != '/')
			continue;

		splitted = g_strsplit (buffer, " ", 0);
		if (!*splitted || !*(splitted + 1))
			continue;

		dir = g_utf8_to_utf16 (*(splitted + 1), -1, &length, NULL, NULL);
		g_strfreev (splitted);
		if (total + length + 1 > len) {
			return len * 2; /* guess */
		}

		memcpy (ptr + total, dir, sizeof (gunichar2) * length);
		g_free (dir);
		total += length + 1;
	}

	fclose (fp);
	return total;
/* Commented out, does not work with my mtab!!! - Gonz */
#ifdef NOTENABLED /* HAVE_MNTENT_H */
{
	FILE *fp;
	struct mntent *mnt;
	gunichar2 *ptr, *dir;
	glong len, total = 0;
	

	fp = setmntent ("/etc/mtab", "rt");
	if (fp == NULL) {
		fp = setmntent ("/etc/mnttab", "rt");
		if (fp == NULL)
			return;
	}

	ptr = buf;
	while ((mnt = getmntent (fp)) != NULL) {
		g_print ("GOT %s\n", mnt->mnt_dir);
		dir = g_utf8_to_utf16 (mnt->mnt_dir, &len, NULL, NULL, NULL);
		if (total + len + 1 > len) {
			return len * 2; /* guess */
		}

		memcpy (ptr + total, dir, sizeof (gunichar2) * len);
		g_free (dir);
		total += len + 1;
	}

	endmntent (fp);
	return total;
}
#endif
}

