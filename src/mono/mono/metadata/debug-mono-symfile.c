#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/rawbuffer.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/debug-mono-symfile.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/mono-debug-debugger.h>

#include <fcntl.h>
#include <unistd.h>

#define RANGE_TABLE_CHUNK_SIZE		256
#define CLASS_TABLE_CHUNK_SIZE		256
#define TYPE_TABLE_PTR_CHUNK_SIZE	256
#define TYPE_TABLE_CHUNK_SIZE		65536

static void
free_method_info (MonoDebugMethodInfo *minfo)
{
	g_free (minfo);
}

static gchar *
get_class_name (MonoClass *klass)
{
	MonoClass *nested_in = mono_class_get_nesting_type (klass);
	const char *name_space;
	if (nested_in) {
		gchar *parent_name = get_class_name (nested_in);
		gchar *name = g_strdup_printf ("%s.%s", parent_name, mono_class_get_name (klass));
		g_free (parent_name);
		return name;
	}

	name_space = mono_class_get_namespace (klass);
	return g_strdup_printf ("%s%s%s", name_space,
				name_space [0] ? "." : "", mono_class_get_name (klass));
}

static int
load_symfile (MonoDebugHandle *handle, MonoSymbolFile *symfile)
{
	const char *ptr, *start;
	gchar *guid;
	guint64 magic;
	long version;

	ptr = start = symfile->raw_contents;
	if (!ptr)
		return FALSE;

	magic = read64(ptr);
	ptr += sizeof(guint64);
	if (magic != MONO_SYMBOL_FILE_MAGIC) {
		g_warning ("Symbol file %s is not a mono symbol file", symfile->filename);
		return FALSE;
	}

	version = read32(ptr);
	ptr += sizeof(guint32);
	if (version != MONO_SYMBOL_FILE_VERSION) {
		g_warning ("Symbol file %s has incorrect version "
			   "(expected %d, got %ld)", symfile->filename,
			   MONO_SYMBOL_FILE_VERSION, version);
		return FALSE;
	}

	guid = mono_guid_to_string ((const guint8 *) ptr);
	ptr += 16;

	if (strcmp (handle->image->guid, guid)) {
		g_warning ("Symbol file %s doesn't match image %s", symfile->filename,
			   handle->image_file);
		return FALSE;
	}

	symfile->offset_table = (MonoSymbolFileOffsetTable *) ptr;

	symfile->method_hash = g_hash_table_new_full (
		g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) free_method_info);

	return TRUE;
}

MonoSymbolFile *
mono_debug_open_mono_symbol_file (MonoDebugHandle *handle, gboolean create_symfile)
{
	MonoSymbolFile *symfile;
	FILE* f;

	mono_debugger_lock ();
	symfile = g_new0 (MonoSymbolFile, 1);

	symfile->filename = g_strdup_printf ("%s.mdb", mono_image_get_filename (handle->image));

	if ((f = fopen (symfile->filename, "rb")) > 0) {
		struct stat stat_buf;
			
		if (fstat (fileno (f), &stat_buf) < 0) {
			g_warning ("stat of %s failed: %s", symfile->filename,  g_strerror (errno));
		} else {	
			symfile->raw_contents_size = stat_buf.st_size;
			symfile->raw_contents = mono_raw_buffer_load (fileno (f), FALSE, 0, stat_buf.st_size);
		}
		
		fclose (f);
	}
	
	if (load_symfile (handle, symfile)) {
		mono_debugger_unlock ();
		return symfile;
	} else if (!create_symfile) {
		mono_debug_close_mono_symbol_file (symfile);
		mono_debugger_unlock ();
		return NULL;
	}

	mono_debugger_unlock ();
	return symfile;
}

void
mono_debug_close_mono_symbol_file (MonoSymbolFile *symfile)
{
	if (!symfile)
		return;

	mono_debugger_lock ();
	if (symfile->method_hash)
		g_hash_table_destroy (symfile->method_hash);

	if (symfile->raw_contents)
		mono_raw_buffer_free ((gpointer) symfile->raw_contents);
	
	g_free (symfile);
	mono_debugger_unlock ();
}

static int
read_leb128 (const char *ptr, const char **rptr)
{
	int ret = 0;
	int shift = 0;
	char b;

	do {
		b = *ptr++;
				
		ret = ret | ((b & 0x7f) << shift);
		shift += 7;
	} while ((b & 0x80) == 0x80);

	if (rptr)
		*rptr = ptr;

	return ret;
}

static gchar *
read_string (const char *ptr)
{
	int len = read_leb128 (ptr, &ptr);
	return g_filename_from_utf8 (ptr, len, NULL, NULL, NULL);
}

gchar *
mono_debug_find_source_location (MonoSymbolFile *symfile, MonoMethod *method, guint32 offset,
				 guint32 *line_number)
{
	MonoSymbolFileLineNumberEntry *lne;
	MonoDebugMethodInfo *minfo;
	gchar *source_file = NULL;
	const char *ptr;
	int i;

	mono_debugger_lock ();
	if (!symfile->method_hash) {
		mono_debugger_unlock ();
		return NULL;
	}

	minfo = g_hash_table_lookup (symfile->method_hash, method);
	if (!minfo) {
		mono_debugger_unlock ();
		return NULL;
	}

	if (read32(&(minfo->entry->_source_index))) {
		int offset = read32(&(symfile->offset_table->_source_table_offset)) +
			(read32(&(minfo->entry->_source_index)) - 1) * sizeof (MonoSymbolFileSourceEntry);
		MonoSymbolFileSourceEntry *se = (MonoSymbolFileSourceEntry *) (symfile->raw_contents + offset);

		source_file = read_string (symfile->raw_contents + read32(&(se->_name_offset)));
	}

	ptr = symfile->raw_contents + read32(&(minfo->entry->_line_number_table_offset));

	lne = (MonoSymbolFileLineNumberEntry *) ptr;

	for (i = 0; i < read32(&(minfo->entry->_num_line_numbers)); i++, lne++) {
		if (read32(&(lne->_offset)) < offset)
			continue;

		if (line_number) {
			*line_number = read32(&(lne->_row));
			mono_debugger_unlock ();
			if (source_file)
				return source_file;
			else
				return NULL;
		} else if (source_file) {
			gchar *retval = g_strdup_printf ("%s:%d", source_file, read32(&(lne->_row)));
			g_free (source_file);
			mono_debugger_unlock ();
			return retval;
		} else {
			gchar* retval = g_strdup_printf ("%d", read32(&(lne->_row)));
			mono_debugger_unlock ();
			return retval;
		}
	}

	mono_debugger_unlock ();
	return NULL;
}

gint32
_mono_debug_address_from_il_offset (MonoDebugMethodJitInfo *jit, guint32 il_offset)
{
	int i;

	if (!jit || !jit->line_numbers)
		return -1;

	for (i = jit->num_line_numbers - 1; i >= 0; i--) {
		MonoDebugLineNumberEntry lne = jit->line_numbers [i];

		if (lne.il_offset <= il_offset)
			return lne.native_offset;
	}

	return -1;
}

static int
compare_method (const void *key, const void *object)
{
	guint32 token = GPOINTER_TO_UINT (key);
	MonoSymbolFileMethodIndexEntry *me = (MonoSymbolFileMethodIndexEntry*)object;

	return token - read32(&(me->_token));
}

MonoDebugMethodInfo *
mono_debug_find_method (MonoDebugHandle *handle, MonoMethod *method)
{
	MonoSymbolFileMethodEntry *me;
	MonoSymbolFileMethodIndexEntry *first_ie, *ie;
	MonoDebugMethodInfo *minfo;
	MonoSymbolFile *symfile = handle->symfile;

	if (!symfile->method_hash)
		return NULL;

	if (handle->image != mono_class_get_image (mono_method_get_class (method)))
		return NULL;

	mono_debugger_lock ();
	first_ie = (MonoSymbolFileMethodIndexEntry *)
		(symfile->raw_contents + read32(&(symfile->offset_table->_method_table_offset)));

	ie = bsearch (GUINT_TO_POINTER (mono_method_get_token (method)), first_ie,
				   read32(&(symfile->offset_table->_method_count)),
				   sizeof (MonoSymbolFileMethodIndexEntry), compare_method);

	if (!ie) {
		mono_debugger_unlock ();
		return NULL;
	}

	me = (MonoSymbolFileMethodEntry *) (symfile->raw_contents + read32(&(ie->_file_offset)));

	minfo = g_new0 (MonoDebugMethodInfo, 1);
	minfo->index = (ie - first_ie) + 1;
	minfo->method = method;
	minfo->handle = handle;
	minfo->num_il_offsets = read32(&(me->_num_line_numbers));
	minfo->il_offsets = (MonoSymbolFileLineNumberEntry *)
		(symfile->raw_contents + read32(&(me->_line_number_table_offset)));
	minfo->num_lexical_blocks = read32(&(me->_num_lexical_blocks));
	minfo->lexical_blocks = (MonoSymbolFileLexicalBlockEntry *)
		(symfile->raw_contents + read32(&(me->_lexical_block_table_offset)));
	minfo->entry = me;

	g_hash_table_insert (symfile->method_hash, method, minfo);

	mono_debugger_unlock ();
	return minfo;
}
