/*
 * rawbuffer.c: Manages buffers that might have been mmapped or malloced
 *
 * Author:
 *   Miguel de Icaza (miguel@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */
#include <config.h>
#if defined(PLATFORM_WIN32)
#define USE_WIN32_API		1
#endif

#include <unistd.h>
#include <errno.h>
#ifdef USE_WIN32_API
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#endif
#include <sys/types.h>
#include <glib.h>
#include "rawbuffer.h"

#include <mono/io-layer/io-layer.h>

#define ROUND_DOWN(VALUE,SIZE)	((VALUE) & ~((SIZE) - 1))
#define ROUND_UP(VALUE,SIZE)	(ROUND_DOWN((VALUE) + (SIZE) - 1, (SIZE)))
#if SIZEOF_VOID_P == 8
#define UINTPTR_TYPE guint64
#else
#define UINTPTR_TYPE guint32
#endif

static GHashTable *mmap_map = NULL;
static size_t alignment = 0;
static CRITICAL_SECTION mmap_mutex;

static void
get_alignment (void)
{
#ifdef USE_WIN32_API
	SYSTEM_INFO info;

	GetSystemInfo (&info);
	alignment = info.dwAllocationGranularity;
#else
	alignment = getpagesize ();
#endif
}

static void *
mono_raw_buffer_load_malloc (int fd, int is_writable, guint32 base, size_t size)
{
	void *ptr;

	ptr = g_malloc (size);
	if (ptr == NULL)
		return NULL;

	if (lseek (fd, base, 0) == (off_t) -1) {
		g_free (ptr);
		return NULL;
	}

	read (fd, ptr, size);
	return ptr;
}

static void
mono_raw_buffer_free_malloc (void *base)
{
	g_free (base);
}

void
mono_raw_buffer_init (void)
{
	InitializeCriticalSection (&mmap_mutex);

	get_alignment ();

	mmap_map = g_hash_table_new (NULL, NULL);
}

static void *
mono_raw_buffer_load_mmap (int fd, int is_writable, guint32 base, size_t size)
{
#ifdef USE_WIN32_API
	/* FileMapping implementation */

	DWORD start, end;
	int prot, access;
	void *ptr;
	HANDLE file, mapping;

	start = ROUND_DOWN (base, alignment);
	end = base + size;
	
	if (is_writable) {
		prot = PAGE_WRITECOPY;
		access = FILE_MAP_COPY;
	}
	else {
		prot = PAGE_READONLY;
		access = FILE_MAP_READ;
	}

	file = (HANDLE) _get_osfhandle (fd);
	mapping = CreateFileMapping (file, NULL, prot, 0, 0, NULL);
	if (mapping == NULL)
		return 0;

	ptr = MapViewOfFile (mapping, access, 0, start, end - start);
	if (ptr == NULL) {
		CloseHandle (mapping);
		return 0;
	}

	EnterCriticalSection (&mmap_mutex);
	g_hash_table_insert (mmap_map, ptr, GINT_TO_POINTER (mapping));
	LeaveCriticalSection (&mmap_mutex);
	
	return ((char *)ptr) + (base - start);

#else
	/* mmap implementation */


	size_t start, end;
	int prot = PROT_READ;
	int flags = 0;
	void *ptr;

	start = ROUND_DOWN (base, alignment);
	end = ROUND_UP (base + size, alignment);

	if (is_writable){
		prot |= PROT_WRITE;
		flags = MAP_SHARED;
	} else {
		flags = MAP_PRIVATE;
	}

	ptr = mmap (0, end - start, prot, flags, fd, start);

	if (ptr == (void *) -1)
		return 0;

	/* 
	 * This seems to prevent segmentation faults on Fedora Linux, no
	 * idea why :). See
	 * http://bugzilla.ximian.com/show_bug.cgi?id=49499
	 * for more info.
	 */
	if (mprotect (ptr, end - start, prot | PROT_EXEC) != 0)
		g_warning (G_GNUC_PRETTY_FUNCTION
				   ": mprotect failed: %s", g_strerror (errno));
	
	EnterCriticalSection (&mmap_mutex);
	g_hash_table_insert (mmap_map, ptr, GINT_TO_POINTER (size));
	LeaveCriticalSection (&mmap_mutex);

	return ((char *)ptr) + (base - start);
#endif
}

static void
mono_raw_buffer_free_mmap (void *base)
{
	int value;

	EnterCriticalSection (&mmap_mutex);
	value = GPOINTER_TO_INT (g_hash_table_lookup (mmap_map, base));
	LeaveCriticalSection (&mmap_mutex);

#ifdef USE_WIN32_API
	UnmapViewOfFile (base);
	CloseHandle ((HANDLE) value);
#else
	munmap (base, value);
#endif
}

static void
mono_raw_buffer_update_mmap (void *base, size_t size)
{
#ifdef USE_WIN32_API
	FlushViewOfFile (base, size);
#else
	msync (base, size, MS_SYNC);
#endif
}

void *
mono_raw_buffer_load (int fd, int is_writable, guint32 base, size_t size)
{
	void *ptr;

	ptr = mono_raw_buffer_load_mmap (fd, is_writable, base, size);
	if (ptr == 0)
		ptr = mono_raw_buffer_load_malloc (fd, is_writable, base, size);
	
	return ptr;
}

void
mono_raw_buffer_update (void *buffer, size_t size)
{
	char *mmap_base;
	gboolean exists;

	mmap_base =  (gpointer)(ROUND_DOWN ((UINTPTR_TYPE) (buffer), alignment));

	EnterCriticalSection (&mmap_mutex);
	exists = g_hash_table_lookup (mmap_map, mmap_base) != NULL;
	LeaveCriticalSection (&mmap_mutex);
	if (exists)
		mono_raw_buffer_update_mmap (mmap_base, size);
}

void
mono_raw_buffer_free (void *buffer)
{
	char *mmap_base;
	gboolean exists;

	mmap_base = (gpointer)(ROUND_DOWN ((UINTPTR_TYPE) (buffer), alignment));
	
	exists = g_hash_table_lookup (mmap_map, mmap_base) != NULL;
	if (exists)
		mono_raw_buffer_free_mmap (mmap_base);
	else
		mono_raw_buffer_free_malloc (buffer);
}

