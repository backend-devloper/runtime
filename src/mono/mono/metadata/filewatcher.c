/*
 * filewatcher.c: File System Watcher internal calls
 *
 * Authors:
 *	Gonzalo Paniagua Javier (gonzalo@ximian.com)
 *
 * (C) 2004 Novell, Inc. (http://www.novell.com)
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <mono/metadata/appdomain.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/filewatcher.h>

#if (defined (PLATFORM_WIN32) && WINVER >= 0x0400)
/* Supported under windows */
gint
ves_icall_System_IO_FSW_SupportsFSW (void)
{
	return 1;
}

gpointer
ves_icall_System_IO_FSW_OpenDirectory (MonoString *path, gpointer reserved)
{
	gpointer dir;
	gchar *utf8path;

	MONO_ARCH_SAVE_REGS;

	utf8path = mono_string_to_utf8 (path);
	dir = CreateFile (path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
			  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	g_free (utf8path);

	return dir;
}

gboolean
ves_icall_System_IO_FSW_CloseDirectory (gpointer handle)
{
	MONO_ARCH_SAVE_REGS;

	return CloseHandle (handle);
}

gboolean
ves_icall_System_IO_FSW_ReadDirectoryChanges (  gpointer handle,
						MonoArray *buffer,
						gboolean includeSubdirs,
						gint filters,
						gpointer overlap,
						gpointer callback)
{
	gpointer dest;
	gint size;
	MonoObject *delegate = (MonoObject *) callback;
	MonoMethod *im;
	LPOVERLAPPED_COMPLETION_ROUTINE func;

	MONO_ARCH_SAVE_REGS;

	size = mono_array_length (buffer);
	dest = mono_array_addr_with_size (buffer, 1, 0);

	im = mono_get_delegate_invoke (delegate->vtable->klass);
	func = mono_compile_method (im);
	return ReadDirectoryChanges (handle, dest, size, includeSubdirs, filters,
				     NULL, (LPOVERLAPPED) overlap,
				     func);
}

gboolean
ves_icall_System_IO_FAMW_InternalFAMNextEvent (gpointer conn,
						MonoString **filename,
						gint *code,
						gint *reqnum)
{
	return FALSE;
}
#else

static int (*FAMNextEvent) (gpointer, gpointer);

gint
ves_icall_System_IO_FSW_SupportsFSW (void)
{
	GModule *fam_module;

	MONO_ARCH_SAVE_REGS;

	fam_module = g_module_open ("libfam", G_MODULE_BIND_LAZY);
	if (fam_module == NULL) {
		return 0;
	}

	
	g_module_symbol (fam_module, "FAMNextEvent", (gpointer *) &FAMNextEvent);
	if (FAMNextEvent == NULL)
		return 0;

	return 2;
}

gpointer
ves_icall_System_IO_FSW_OpenDirectory (MonoString *path, gpointer reserved)
{
	return NULL;
}

gboolean
ves_icall_System_IO_FSW_CloseDirectory (gpointer handle)
{
	return FALSE;
}

gboolean
ves_icall_System_IO_FSW_ReadDirectoryChanges (	gpointer handle,
						MonoArray *buffer,
						gboolean includeSubdirs,
						gint filters,
						gpointer overlap,
						gpointer callback)
{
	return FALSE;
}

/* Almost copied from fam.h. Weird, I know */
typedef struct {
	gint reqnum;
} FAMRequest;

typedef struct FAMEvent {
    gpointer fc;
    FAMRequest fr;
    gchar *hostname;
    gchar filename [PATH_MAX];
    gpointer userdata;
    gint code;
} FAMEvent;

gboolean
ves_icall_System_IO_FAMW_InternalFAMNextEvent (gpointer conn,
						MonoString **filename,
						gint *code,
						gint *reqnum)
{
	FAMEvent ev;

	MONO_ARCH_SAVE_REGS;

	if (FAMNextEvent (conn, &ev) == 1) {
		*filename = mono_string_new (mono_domain_get (), ev.filename);
		*code = ev.code;
		*reqnum = ev.fr.reqnum;
		return TRUE;
	}

	return FALSE;
}

#endif

