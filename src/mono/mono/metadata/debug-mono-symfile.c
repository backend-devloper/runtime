#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/rawbuffer.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/debug-mono-symfile.h>

#include <fcntl.h>
#include <unistd.h>

#define RANGE_TABLE_CHUNK_SIZE		256
#define CLASS_TABLE_CHUNK_SIZE		256
#define TYPE_TABLE_PTR_CHUNK_SIZE	256
#define TYPE_TABLE_CHUNK_SIZE		65536

struct MonoSymbolFilePriv
{
	int fd;
	int error;
	char *file_name;
	char *source_file;
	int temp_idx;
	guint32 string_table_size;
	guint32 string_offset_size;
	MonoImage *image;
	GHashTable *method_table;
	GHashTable *method_hash;
	MonoSymbolFileOffsetTable *offset_table;
};

typedef struct
{
	MonoMethod *method;
	MonoDebugMethodInfo *minfo;
	MonoSymbolFileMethodEntry *entry;
	guint32 method_name_offset;
	guint32 index;
	gchar *name;
} MonoSymbolFileMethodEntryPriv;

static GHashTable *type_table;
static GHashTable *class_table;

MonoGlobalSymbolFile *mono_debugger_global_symbol_file = NULL;

static int write_string_table (MonoSymbolFile *symfile);
static int create_symfile (MonoSymbolFile *symfile, gboolean emit_warnings);
static void close_symfile (MonoSymbolFile *symfile);
static MonoDebugRangeInfo *allocate_range_entry (MonoSymbolFile *symfile);
static MonoDebugClassInfo *allocate_class_entry (MonoSymbolFile *symfile);
static guint32 allocate_type_entry (MonoGlobalSymbolFile *global_symfile, guint32 size, guint8 **ptr);
static guint32 write_type (MonoGlobalSymbolFile *global_symfile, MonoType *type);

static void
free_method_info (MonoDebugMethodInfo *minfo)
{
	g_free (minfo->jit);
	g_free (minfo);
}

static gchar *
get_class_name (MonoClass *klass)
{
	if (klass->nested_in) {
		gchar *parent_name = get_class_name (klass->nested_in);
		gchar *name = g_strdup_printf ("%s.%s", parent_name, klass->name);
		g_free (parent_name);
		return name;
	}

	return g_strdup_printf ("%s%s%s", klass->name_space,
				klass->name_space [0] ? "." : "", klass->name);
}

static gchar *
get_method_name (MonoMethod *method)
{
	gchar *tmpsig = mono_signature_get_desc (method->signature, TRUE);
	gchar *class_name = get_class_name (method->klass);
	gchar *name = g_strdup_printf ("%s.%s(%s)", class_name, method->name, tmpsig);
	g_free (class_name);
	g_free (tmpsig);
	return name;
}

static int
load_symfile (MonoSymbolFile *symfile)
{
	MonoSymbolFilePriv *priv = symfile->_priv;
	MonoSymbolFileMethodEntry *me;
	const char *ptr, *start;
	guint64 magic;
	long version;
	int i;

	if (!mono_debugger_global_symbol_file)
		mono_debugger_global_symbol_file = g_new0 (MonoGlobalSymbolFile, 1);
	symfile->global = mono_debugger_global_symbol_file;

	ptr = start = symfile->raw_contents;

	magic = *((guint64 *) ptr)++;
	if (magic != MONO_SYMBOL_FILE_MAGIC) {
		g_warning ("Symbol file %s has is not a mono symbol file", priv->file_name);
		return FALSE;
	}

	version = *((guint32 *) ptr)++;
	if (version != MONO_SYMBOL_FILE_VERSION) {
		g_warning ("Symbol file %s has incorrect line number table version "
			   "(expected %d, got %ld)", priv->file_name,
			   MONO_SYMBOL_FILE_VERSION, version);
		return FALSE;
	}

	priv->offset_table = (MonoSymbolFileOffsetTable *) ptr;

	mono_debug_symfile_add_type (symfile, mono_defaults.object_class);

	/*
	 * Read method table.
	 *
	 */

	priv->method_table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
						    (GDestroyNotify) g_free);
	priv->method_hash = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
						   (GDestroyNotify) free_method_info);

	ptr = symfile->raw_contents + priv->offset_table->method_table_offset;
	me = (MonoSymbolFileMethodEntry *) ptr;

	for (i = 0; i < priv->offset_table->method_count; i++, me++) {
		MonoMethod *method = mono_get_method (priv->image, me->token, NULL);
		MonoSymbolFileMethodEntryPriv *mep;
		MonoDebugMethodInfo *minfo;

		if (!method)
			continue;

		minfo = g_new0 (MonoDebugMethodInfo, 1);
		minfo->file_offset = ((const char *) me) - start;
		minfo->method = method;
		minfo->symfile = symfile;
		minfo->num_il_offsets = me->num_line_numbers;
		minfo->il_offsets = (MonoSymbolFileLineNumberEntry *)
			(symfile->raw_contents + me->line_number_table_offset);

		mep = g_new0 (MonoSymbolFileMethodEntryPriv, 1);
		mep->method = method;
		mep->minfo = minfo;
		mep->entry = me;
		mep->index = i;

		mep->method_name_offset = priv->string_table_size;
		mep->name = get_method_name (method);
		priv->string_table_size += strlen (mep->name) + 5;

		g_hash_table_insert (priv->method_table, method, mep);
		g_hash_table_insert (priv->method_hash, method, minfo);
	}

	if (!write_string_table (symfile))
		return FALSE;

	return TRUE;
}

MonoSymbolFile *
mono_debug_open_mono_symbol_file (MonoImage *image, const char *filename, gboolean emit_warnings)
{
	MonoSymbolFile *symfile;
	MonoSymbolFilePriv *priv;
	off_t file_size;
	void *ptr;
	int fd;

	fd = open (filename, O_RDONLY);
	if (fd == -1) {
		if (emit_warnings)
			g_warning ("Can't open symbol file: %s", filename);
		return NULL;
	}

	file_size = lseek (fd, 0, SEEK_END);
	lseek (fd, 0, SEEK_SET);

	if (file_size == (off_t) -1) {
		if (emit_warnings)
			g_warning ("Can't get size of symbol file: %s", filename);
		return NULL;
	}

	ptr = mono_raw_buffer_load (fd, FALSE, 0, file_size);
	if (!ptr) {
		if (emit_warnings)
			g_warning ("Can't read symbol file: %s", filename);
		return NULL;
	}

	symfile = g_new0 (MonoSymbolFile, 1);
	symfile->magic = MONO_SYMBOL_FILE_MAGIC;
	symfile->version = MONO_SYMBOL_FILE_VERSION;
	symfile->dynamic_magic = MONO_SYMBOL_FILE_DYNAMIC_MAGIC;
	symfile->dynamic_version = MONO_SYMBOL_FILE_DYNAMIC_VERSION;
	symfile->image_file = g_strdup (image->name);
	symfile->symbol_file = g_strdup (filename);
	symfile->raw_contents = ptr;
	symfile->raw_contents_size = file_size;

	symfile->_priv = priv = g_new0 (MonoSymbolFilePriv, 1);

	priv->fd = fd;
	priv->image = image;
	priv->file_name = g_strdup (filename);

	if (!load_symfile (symfile)) {
		mono_debug_close_mono_symbol_file (symfile);
		return NULL;
	}

	return symfile;
}

static void
close_symfile (MonoSymbolFile *symfile)
{
	MonoSymbolFilePriv *priv = symfile->_priv;

	if (symfile->raw_contents) {
		mono_raw_buffer_free (symfile->raw_contents);
		symfile->raw_contents = NULL;
	}

	if (priv->fd) {
		close (priv->fd);
		priv->fd = 0;
	}

	if (priv->method_table) {
		g_hash_table_destroy (priv->method_table);
		priv->method_table = NULL;
	}

	if (symfile->is_dynamic) {
		unlink (priv->file_name);
		priv->method_hash = NULL;
	} else if (priv->method_hash) {
		g_hash_table_destroy (priv->method_hash);
		priv->method_hash = NULL;
	}

	if (symfile->image_file) {
		g_free (symfile->image_file);
		symfile->image_file = NULL;
	}

	if (priv->file_name) {
		g_free (priv->file_name);
		priv->file_name = NULL;
	}

	priv->error = FALSE;
}

void
mono_debug_close_mono_symbol_file (MonoSymbolFile *symfile)
{
	if (!symfile)
		return;

	close_symfile (symfile);

	g_free (symfile->_priv->source_file);
	g_free (symfile->_priv);
	g_free (symfile->image_file);
	g_free (symfile->symbol_file);
	g_free (symfile);
}

static gchar *
read_string (const char *ptr)
{
	int len = *((guint32 *) ptr)++;

	return g_filename_from_utf8 (ptr, len, NULL, NULL, NULL);
}

gchar *
mono_debug_find_source_location (MonoSymbolFile *symfile, MonoMethod *method, guint32 offset,
				 guint32 *line_number)
{
	MonoSymbolFilePriv *priv = symfile->_priv;
	MonoSymbolFileLineNumberEntry *lne;
	MonoSymbolFileMethodEntryPriv *mep;
	gchar *source_file = NULL;
	const char *ptr;
	int i;

	if (!priv->method_table || symfile->is_dynamic)
		return NULL;

	mep = g_hash_table_lookup (priv->method_table, method);
	if (!mep)
		return NULL;

	if (mep->entry->source_file_offset)
		source_file = read_string (symfile->raw_contents + mep->entry->source_file_offset);

	ptr = symfile->raw_contents + mep->entry->line_number_table_offset;

	lne = (MonoSymbolFileLineNumberEntry *) ptr;

	for (i = 0; i < mep->entry->num_line_numbers; i++, lne++) {
		if (lne->offset < offset)
			continue;

		if (line_number) {
			*line_number = lne->row;
			if (source_file)
				return source_file;
			else
				return NULL;
		} else if (source_file) {
			gchar *retval = g_strdup_printf ("%s:%d", source_file, lne->row);
			g_free (source_file);
			return retval;
		} else
			return g_strdup_printf ("%d", lne->row);
	}

	return NULL;
}

void
mono_debug_symfile_add_method (MonoSymbolFile *symfile, MonoMethod *method)
{
	MonoSymbolFileMethodEntryPriv *mep;
	MonoSymbolFileMethodAddress *address;
	MonoDebugVarInfo *var_table;
	MonoDebugRangeInfo *range;
	MonoMethodHeader *header;
	guint32 size, num_variables, variable_size, variable_offset;
	guint32 type_size, type_offset, *type_index_table;
	guint32 line_size, line_offset;
	MonoDebugLineNumberEntry *line_table;
	guint32 *type_table;
	guint8 *ptr;
	int i;

	if (!symfile->_priv->method_table)
		return;

	header = ((MonoMethodNormal *) method)->header;

	mep = g_hash_table_lookup (symfile->_priv->method_table, method);
	if (!mep)
		return;

	if (!mep->minfo) {
		mep->minfo = g_hash_table_lookup (symfile->_priv->method_hash, mep->method);
		if (!mep->minfo)
			return;
	}

	if (!mep->minfo->jit)
		return;

	symfile->generation++;

	size = sizeof (MonoSymbolFileMethodAddress);

	num_variables = mep->entry->num_parameters + mep->entry->num_locals;
	if (mep->entry->this_type_index)
		num_variables++;

	variable_size = num_variables * sizeof (MonoDebugVarInfo);
	variable_offset = size;
	size += variable_size;

	type_size = num_variables * sizeof (gpointer);
	type_offset = size;
	size += type_size;

	if (mep->minfo->jit->line_numbers) {
		line_offset = size;
		line_size = mep->minfo->jit->line_numbers->len * sizeof (MonoDebugLineNumberEntry);
		size += line_size;
	}

	address = g_malloc0 (size);
	ptr = (guint8 *) address;

	address->size = size;
	address->start_address = mep->minfo->jit->code_start;
	address->end_address = mep->minfo->jit->code_start + mep->minfo->jit->code_size;
	address->method_start_address = address->start_address + mep->minfo->jit->prologue_end;
	address->method_end_address = address->start_address + mep->minfo->jit->epilogue_begin;
	address->wrapper_address = mep->minfo->jit->wrapper_addr;
	address->variable_table_offset = variable_offset;
	address->type_table_offset = type_offset;

	if (mep->minfo->jit->line_numbers) {
		address->num_line_numbers = mep->minfo->jit->line_numbers->len;
		address->line_number_offset = line_offset;

		line_table = (MonoDebugLineNumberEntry *) (ptr + line_offset);
		memcpy (line_table, mep->minfo->jit->line_numbers->data, line_size);
	}

	range = allocate_range_entry (symfile);
	range->file_offset = mep->minfo->file_offset;
	range->start_address = address->start_address;
	range->end_address = address->end_address;
	range->dynamic_data = address;
	range->dynamic_size = size;

	var_table = (MonoDebugVarInfo *) (ptr + variable_offset);
	type_table = (guint32 *) (ptr + type_offset);

	type_index_table = (guint32 *)
		(symfile->raw_contents + mep->entry->type_index_table_offset);

	if (mep->entry->this_type_index) {
		if (!mep->minfo->jit->this_var) {
			g_warning (G_STRLOC ": Method %s.%s doesn't have `this'.",
				   mep->method->klass->name, mep->method->name);
			var_table++;
		} else {
			*var_table++ = *mep->minfo->jit->this_var;
			*type_table++ = write_type (symfile->global, &method->klass->this_arg);
		}
	}

	if (mep->minfo->jit->num_params != mep->entry->num_parameters) {
		g_warning (G_STRLOC ": Method %s.%s has %d parameters, but symbol file claims it has %d.",
			   mep->method->klass->name, mep->method->name, mep->minfo->jit->num_params,
			   mep->entry->num_parameters);
		G_BREAKPOINT ();
		var_table += mep->entry->num_parameters;
	} else {
		for (i = 0; i < mep->minfo->jit->num_params; i++) {
			*var_table++ = mep->minfo->jit->params [i];
			*type_table++ = write_type (symfile->global, method->signature->params [i]);
		}
	}

	if (mep->minfo->jit->num_locals < mep->entry->num_locals) {
#if 1
		g_warning (G_STRLOC ": Method %s.%s has %d locals, but symbol file claims it has %d.",
			   mep->method->klass->name, mep->method->name, mep->minfo->jit->num_locals,
			   mep->entry->num_locals);
#endif
		var_table += mep->entry->num_locals;
	} else {
		g_assert ((header != NULL) || (mep->entry->num_locals == 0));
		for (i = 0; i < mep->entry->num_locals; i++) {
			MonoDebugVarInfo *var = &mep->minfo->jit->locals [i];
			*var_table++ = mep->minfo->jit->locals [i];
			*type_table++ = write_type (symfile->global, header->locals [i]);
		}
	}
}

void
mono_debug_symfile_add_type (MonoSymbolFile *symfile, MonoClass *klass)
{
	MonoDebugClassInfo *info;

	if (!class_table)
		class_table = g_hash_table_new (g_direct_hash, g_direct_equal);

	/* We write typeof (object) into each symbol file's type table. */
	if ((klass != mono_defaults.object_class) && g_hash_table_lookup (class_table, klass))
		return;

	symfile->generation++;

	info = allocate_class_entry (symfile);
	info->klass = klass;
	if (klass->rank) {
		info->token = klass->element_class->type_token;
		info->rank = klass->rank;
	} else
		info->token = klass->type_token;
	info->type_info = write_type (symfile->global, &klass->this_arg);
}

static int
create_symfile (MonoSymbolFile *symfile, gboolean emit_warnings)
{
	MonoSymbolFilePriv *priv = symfile->_priv;
	char *ptr;
	guint64 magic;
	long version;
	off_t offset;

	if (!mono_debugger_global_symbol_file)
		mono_debugger_global_symbol_file = g_new0 (MonoGlobalSymbolFile, 1);
	symfile->global = mono_debugger_global_symbol_file;

	priv->fd = g_file_open_tmp (NULL, &priv->file_name, NULL);
	if (priv->fd == -1) {
		if (emit_warnings)
			g_warning ("Can't create symbol file");
		return FALSE;
	}

	symfile->symbol_file = g_strdup (priv->file_name);

	magic = MONO_SYMBOL_FILE_MAGIC;
	if (write (priv->fd, &magic, sizeof (magic)) < 0)
		return FALSE;

	version = MONO_SYMBOL_FILE_VERSION;
	if (write (priv->fd, &version, sizeof (version)) < 0)
		return FALSE;

	offset = lseek (priv->fd, 0, SEEK_CUR);

	priv->offset_table = g_new0 (MonoSymbolFileOffsetTable, 1);
	if (write (priv->fd, priv->offset_table, sizeof (MonoSymbolFileOffsetTable)) < 0)
		return FALSE;

	mono_debug_symfile_add_type (symfile, mono_defaults.object_class);

	//
	// Write offset table.
	//

	symfile->raw_contents_size = lseek (priv->fd, 0, SEEK_CUR);

	lseek (priv->fd, offset, SEEK_SET);
	if (write (priv->fd, priv->offset_table, sizeof (MonoSymbolFileOffsetTable)) < 0)
		return FALSE;

	lseek (priv->fd, symfile->raw_contents_size, SEEK_SET);

	ptr = mono_raw_buffer_load (priv->fd, TRUE, 0, symfile->raw_contents_size);
	if (!ptr)
		return FALSE;

	symfile->raw_contents = ptr;

	return TRUE;
}

MonoSymbolFile *
mono_debug_create_mono_symbol_file (MonoImage *image)
{
	MonoSymbolFile *symfile;

	symfile = g_new0 (MonoSymbolFile, 1);
	symfile->magic = MONO_SYMBOL_FILE_MAGIC;
	symfile->version = MONO_SYMBOL_FILE_VERSION;
	symfile->dynamic_magic = MONO_SYMBOL_FILE_DYNAMIC_MAGIC;
	symfile->dynamic_version = MONO_SYMBOL_FILE_DYNAMIC_VERSION;
	symfile->is_dynamic = TRUE;
	symfile->image_file = g_strdup (image->name);

	symfile->_priv = g_new0 (MonoSymbolFilePriv, 1);
	symfile->_priv->image = image;

	if (!create_symfile (symfile, TRUE)) {
		mono_debug_close_mono_symbol_file (symfile);
		return NULL;
	}

	return symfile;
}

MonoDebugMethodInfo *
mono_debug_find_method (MonoSymbolFile *symfile, MonoMethod *method)
{
	if (!symfile->_priv->method_hash)
		return NULL;
	else
		return g_hash_table_lookup (symfile->_priv->method_hash, method);
}

static void
write_method_name (gpointer key, gpointer value, gpointer user_data)
{
	MonoSymbolFile *symfile = (MonoSymbolFile *) user_data;
	MonoSymbolFileMethodEntryPriv *mep = (MonoSymbolFileMethodEntryPriv *) value;
	MonoSymbolFilePriv *priv = symfile->_priv;
	guint8 *offset_ptr, *string_ptr;
	guint32 offset;

	offset = mep->method_name_offset + priv->string_offset_size;

	offset_ptr = symfile->string_table + mep->index * 4;
	string_ptr = symfile->string_table + offset;

	*((guint32 *) offset_ptr) = offset;
	*((guint32 *) string_ptr)++ = strlen (mep->name);
	strcpy (string_ptr, mep->name);
}

static int
write_string_table (MonoSymbolFile *symfile)
{
	MonoSymbolFilePriv *priv = symfile->_priv;

	priv->string_offset_size = priv->offset_table->method_count * 4;

	symfile->string_table_size = priv->string_table_size + priv->string_offset_size;
	symfile->string_table = g_malloc0 (symfile->string_table_size);

	g_hash_table_foreach (symfile->_priv->method_table, write_method_name, symfile);
	return TRUE;
}

MonoReflectionMethod *
ves_icall_MonoDebugger_GetMethod (MonoReflectionAssembly *assembly, guint32 token)
{
	MonoMethod *method;

	method = mono_get_method (assembly->assembly->image, token, NULL);

	return mono_method_get_object (mono_domain_get (), method, NULL);
}

MonoReflectionType *
ves_icall_MonoDebugger_GetType (MonoReflectionAssembly *assembly, guint32 token)
{
	MonoClass *klass;

	klass = mono_class_get (assembly->assembly->image, token);
	if (!klass) {
		g_warning (G_STRLOC ": %x", token);
		return NULL;
	}

	return mono_type_get_object (mono_domain_get (), &klass->byval_arg);
}

MonoReflectionType *
ves_icall_MonoDebugger_GetLocalTypeFromSignature (MonoReflectionAssembly *assembly, MonoArray *signature)
{
	MonoDomain *domain; 
	MonoImage *image;
	MonoType *type;
	const char *ptr;
	int len = 0;

	MONO_CHECK_ARG_NULL (assembly);
	MONO_CHECK_ARG_NULL (signature);

	domain = mono_domain_get();
	image = assembly->assembly->image;

	ptr = mono_array_addr (signature, char, 0);
	g_assert (*ptr++ == 0x07);
	len = mono_metadata_decode_value (ptr, &ptr);
	g_assert (len == 1);

	type = mono_metadata_parse_type (image, MONO_PARSE_LOCAL, 0, ptr, &ptr);

	return mono_type_get_object (domain, type);
}

static MonoDebugRangeInfo *
allocate_range_entry (MonoSymbolFile *symfile)
{
	MonoDebugRangeInfo *retval;
	guint32 size, chunks;

	symfile->range_entry_size = sizeof (MonoDebugRangeInfo);

	if (!symfile->range_table) {
		size = sizeof (MonoDebugRangeInfo) * RANGE_TABLE_CHUNK_SIZE;
		symfile->range_table = g_malloc0 (size);
		symfile->num_range_entries = 1;
		return symfile->range_table;
	}

	if (!((symfile->num_range_entries + 1) % RANGE_TABLE_CHUNK_SIZE)) {
		chunks = (symfile->num_range_entries + 1) / RANGE_TABLE_CHUNK_SIZE;
		size = sizeof (MonoDebugRangeInfo) * RANGE_TABLE_CHUNK_SIZE * (chunks + 1);

		symfile->range_table = g_realloc (symfile->range_table, size);
	}

	retval = symfile->range_table + symfile->num_range_entries;
	symfile->num_range_entries++;
	return retval;
}

static MonoDebugClassInfo *
allocate_class_entry (MonoSymbolFile *symfile)
{
	MonoDebugClassInfo *retval;
	guint32 size, chunks;

	symfile->class_entry_size = sizeof (MonoDebugClassInfo);

	if (!symfile->class_table) {
		size = sizeof (MonoDebugClassInfo) * CLASS_TABLE_CHUNK_SIZE;
		symfile->class_table = g_malloc0 (size);
		symfile->num_class_entries = 1;
		return symfile->class_table;
	}

	if (!((symfile->num_class_entries + 1) % CLASS_TABLE_CHUNK_SIZE)) {
		chunks = (symfile->num_class_entries + 1) / CLASS_TABLE_CHUNK_SIZE;
		size = sizeof (MonoDebugClassInfo) * CLASS_TABLE_CHUNK_SIZE * (chunks + 1);

		symfile->class_table = g_realloc (symfile->class_table, size);
	}

	retval = symfile->class_table + symfile->num_class_entries;
	symfile->num_class_entries++;
	return retval;
}

/*
 * Allocate a new entry of size `size' in the type table.
 * Returns the global offset which is to be used to reference this type and
 * a pointer (in the `ptr' argument) which is to be used to write the type.
 */
static guint32
allocate_type_entry (MonoGlobalSymbolFile *global, guint32 size, guint8 **ptr)
{
	guint32 retval;
	guint8 *data;

	g_assert (size + 4 < TYPE_TABLE_CHUNK_SIZE);
	g_assert (ptr != NULL);

	/* Initialize things if necessary. */
	if (!global->current_type_table) {
		global->current_type_table = g_malloc0 (TYPE_TABLE_CHUNK_SIZE);
		global->type_table_size = TYPE_TABLE_CHUNK_SIZE;
		global->type_table_chunk_size = TYPE_TABLE_CHUNK_SIZE;
		global->type_table_offset = 1;
	}

 again:
	/* First let's check whether there's still enough room in the current_type_table. */
	if (global->type_table_offset + size + 4 < global->type_table_size) {
		retval = global->type_table_offset;
		global->type_table_offset += size + 4;
		data = ((guint8 *) global->current_type_table) + retval - global->type_table_start;
		*((gint32 *) data)++ = size;
		*ptr = data;
		return retval;
	}

	/* Add the current_type_table to the type_tables vector and ... */
	if (!global->type_tables) {
		guint32 tsize = sizeof (gpointer) * TYPE_TABLE_PTR_CHUNK_SIZE;
		global->type_tables = g_malloc0 (tsize);
	}

	if (!((global->num_type_tables + 1) % TYPE_TABLE_PTR_CHUNK_SIZE)) {
		guint32 chunks = (global->num_type_tables + 1) / TYPE_TABLE_PTR_CHUNK_SIZE;
		guint32 tsize = sizeof (gpointer) * TYPE_TABLE_PTR_CHUNK_SIZE * (chunks + 1);

		global->type_tables = g_realloc (global->type_tables, tsize);
	}

	global->type_tables [global->num_type_tables++] = global->current_type_table;

	/* .... allocate a new current_type_table. */
	global->current_type_table = g_malloc0 (TYPE_TABLE_CHUNK_SIZE);
	global->type_table_start = global->type_table_offset = global->type_table_size;
	global->type_table_size += TYPE_TABLE_CHUNK_SIZE;

	goto again;
}

static guint32
write_simple_type (MonoGlobalSymbolFile *global, MonoType *type)
{
	guint8 buffer [BUFSIZ], *ptr = buffer;
	guint32 size, offset;

	if (!type_table)
		type_table = g_hash_table_new (g_direct_hash, g_direct_equal);

	offset = GPOINTER_TO_UINT (g_hash_table_lookup (type_table, type));
	if (offset)
		return offset;

	switch (type->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		*((gint32 *) ptr)++ = 1;
		break;

	case MONO_TYPE_CHAR:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		*((gint32 *) ptr)++ = 2;
		break;

	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_R4:
		*((gint32 *) ptr)++ = 4;
		break;

	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R8:
		*((gint32 *) ptr)++ = 8;
		break;

	case MONO_TYPE_I:
	case MONO_TYPE_U:
		*((gint32 *) ptr)++ = sizeof (void *);
		break;

	case MONO_TYPE_VOID:
		*((gint32 *) ptr)++ = 0;
		break;

	case MONO_TYPE_STRING: {
		MonoString string;

		*((gint32 *) ptr)++ = -8;
		*((guint32 *) ptr)++ = sizeof (MonoString);
		*ptr++ = 1;
		*ptr++ = (guint8*)&string.length - (guint8*)&string;
		*ptr++ = sizeof (string.length);
		*ptr++ = (guint8*)&string.chars - (guint8*)&string;
		break;
	}

	default:
		return 0;
	}

	size = ptr - buffer;
	offset = allocate_type_entry (global, size, &ptr);
	memcpy (ptr, buffer, size);

	g_hash_table_insert (type_table, type, GUINT_TO_POINTER (offset));

	return offset;
}

/*
 * Adds type `type' to the type table and returns its offset.
 */
static guint32
write_type (MonoGlobalSymbolFile *global, MonoType *type)
{
	guint8 buffer [BUFSIZ], *ptr = buffer, *old_ptr;
	GPtrArray *methods = NULL;
	int num_fields = 0, num_properties = 0, num_methods = 0;
	int num_params = 0, kind;
	guint32 size, data_size, offset;
	MonoClass *klass = NULL;

	if (!type_table)
		type_table = g_hash_table_new (g_direct_hash, g_direct_equal);
	if (!class_table)
		class_table = g_hash_table_new (g_direct_hash, g_direct_equal);

	offset = GPOINTER_TO_UINT (g_hash_table_lookup (type_table, type));
	if (offset)
		return offset;

	offset = write_simple_type (global, type);
	if (offset)
		return offset;

	kind = type->type;
	if (kind == MONO_TYPE_OBJECT) {
		klass = mono_defaults.object_class;
		kind = MONO_TYPE_CLASS;
	} else if ((kind == MONO_TYPE_VALUETYPE) || (kind == MONO_TYPE_CLASS)) {
		klass = type->data.klass;
		offset = GPOINTER_TO_UINT (g_hash_table_lookup (class_table, klass));
		if (offset)
			return offset;
	}

	switch (kind) {
	case MONO_TYPE_SZARRAY:
		size = 16;
		break;

	case MONO_TYPE_ARRAY:
		size = 23;
		break;

	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS: {
		GHashTable *method_slots = NULL;
		int i;

		if (klass->init_pending) {
			size = 4;
			break;
		}

		mono_class_init (klass);

		offset = GPOINTER_TO_UINT (g_hash_table_lookup (class_table, klass));
		if (offset)
			return offset;

		if (klass->enumtype) {
			size = 13;
			break;
		}

		for (i = 0; i < klass->field.count; i++)
			if (!(klass->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC))
				++num_fields;

		for (i = 0; i < klass->property.count; i++)
			if (!(klass->properties [i].attrs & FIELD_ATTRIBUTE_STATIC))
				++num_properties;

		method_slots = g_hash_table_new (NULL, NULL);
		methods = g_ptr_array_new ();

		for (i = klass->method.count - 1; i >= 0; i--) {
			MonoMethod *method = klass->methods [i];

			if (strcmp (method->name, ".ctor") == 0 || strcmp (method->name, ".cctor") == 0)
				continue;
			if (method->flags & (METHOD_ATTRIBUTE_STATIC | METHOD_ATTRIBUTE_SPECIAL_NAME))
				continue;
			if (!((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC))
				continue;
			if (g_hash_table_lookup (method_slots, GUINT_TO_POINTER (method->slot)))
				continue;
			g_hash_table_insert (method_slots, GUINT_TO_POINTER (method->slot), method);

			++num_methods;
			num_params += method->signature->param_count;

			g_ptr_array_add (methods, method);
		}

		g_hash_table_destroy (method_slots);

		size = 34 + num_fields * 8 + num_properties * (4 + 2 * sizeof (gpointer)) +
			num_methods * (8 + sizeof (gpointer)) + num_params * 4;

		if (kind == MONO_TYPE_CLASS)
			size += 4;

		break;
	}

	default:
		size = sizeof (int);
		break;
	}

	data_size = size;

	offset = allocate_type_entry (global, data_size, &ptr);
	old_ptr = ptr;

	g_hash_table_insert (type_table, type, GUINT_TO_POINTER (offset));

	switch (kind) {
	case MONO_TYPE_SZARRAY: {
		MonoArray array;

		*((gint32 *) ptr)++ = -size;
		*((guint32 *) ptr)++ = sizeof (MonoArray);
		*ptr++ = 2;
		*ptr++ = (guint8*)&array.max_length - (guint8*)&array;
		*ptr++ = sizeof (array.max_length);
		*ptr++ = (guint8*)&array.vector - (guint8*)&array;
		*((guint32 *) ptr)++ = write_type (global, type->data.type);
		break;
	}

	case MONO_TYPE_ARRAY: {
		MonoArray array;
		MonoArrayBounds bounds;

		*((gint32 *) ptr)++ = -size;
		*((guint32 *) ptr)++ = sizeof (MonoArray);
		*ptr++ = 3;
		*ptr++ = (guint8*)&array.max_length - (guint8*)&array;
		*ptr++ = sizeof (array.max_length);
		*ptr++ = (guint8*)&array.vector - (guint8*)&array;
		*ptr++ = type->data.array->rank;
		*ptr++ = (guint8*)&array.bounds - (guint8*)&array;
		*ptr++ = sizeof (MonoArrayBounds);
		*ptr++ = (guint8*)&bounds.lower_bound - (guint8*)&bounds;
		*ptr++ = sizeof (bounds.lower_bound);
		*ptr++ = (guint8*)&bounds.length - (guint8*)&bounds;
		*ptr++ = sizeof (bounds.length);
		*((guint32 *) ptr)++ = write_type (global, type->data.array->type);
		break;
	}

	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS: {
		int base_offset = kind == MONO_TYPE_CLASS ? 0 : - sizeof (MonoObject);
		int i, j;

		if (klass->init_pending) {
			*((gint32 *) ptr)++ = -1;
			break;
		}

		g_hash_table_insert (class_table, klass, GUINT_TO_POINTER (offset));

		if (klass->enumtype) {
			*((gint32 *) ptr)++ = -size;
			*((guint32 *) ptr)++ = sizeof (MonoObject);
			*ptr++ = 4;
			*((guint32 *) ptr)++ = write_type (global, klass->enum_basetype);
			break;
		}

		*((gint32 *) ptr)++ = -size;

		*((guint32 *) ptr)++ = klass->instance_size + base_offset;
		if (type->type == MONO_TYPE_OBJECT)
			*ptr++ = 7;
		else
			*ptr++ = kind == MONO_TYPE_CLASS ? 6 : 5;
		*ptr++ = kind == MONO_TYPE_CLASS;
		*((guint32 *) ptr)++ = num_fields;
		*((guint32 *) ptr)++ = num_fields * (4 + sizeof (gpointer));
		*((guint32 *) ptr)++ = num_properties;
		*((guint32 *) ptr)++ = num_properties * 3 * sizeof (gpointer);
		*((guint32 *) ptr)++ = num_methods;
		*((guint32 *) ptr)++ = num_methods * (4 + 2 * sizeof (gpointer)) +
			num_params * sizeof (gpointer);
		for (i = 0; i < klass->field.count; i++) {
			if (klass->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			*((guint32 *) ptr)++ = klass->fields [i].offset + base_offset;
			*((guint32 *) ptr)++ = write_type (global, klass->fields [i].type);
		}

		for (i = 0; i < klass->property.count; i++) {
			if (klass->properties [i].attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			if (klass->properties [i].get)
				*((guint32 *) ptr)++ = write_type
					(global, klass->properties [i].get->signature->ret);
			else
				*((guint32 *) ptr)++ = 0;
			*((gpointer *) ptr)++ = klass->properties [i].get;
			*((gpointer *) ptr)++ = klass->properties [i].set;
		}

		for (i = 0; i < methods->len; i++) {
			MonoMethod *method = g_ptr_array_index (methods, i);

			*((gpointer *) ptr)++ = method;
			if ((method->signature->ret) && (method->signature->ret->type != MONO_TYPE_VOID))
				*((guint32 *) ptr)++ = write_type (global, method->signature->ret);
			else
				*((guint32 *) ptr)++ = 0;
			*((guint32 *) ptr)++ = method->signature->param_count;
			for (j = 0; j < method->signature->param_count; j++)
				*((guint32 *) ptr)++ = write_type (global, method->signature->params [j]);
		}

		g_ptr_array_free (methods, FALSE);

		if (kind == MONO_TYPE_CLASS) {
			if (klass->parent)
				*((guint32 *) ptr)++ = write_type (global, &klass->parent->this_arg);
			else
				*((guint32 *) ptr)++ = 0;
		}

		break;
	}

	default:
		g_message (G_STRLOC ": %p - %x,%x,%x", type, type->attrs, kind, type->byref);

		*((gint32 *) ptr)++ = -1;
		break;
	}

	if (ptr - old_ptr != data_size) {
		g_warning (G_STRLOC ": %d,%d - %d", ptr - old_ptr, data_size, kind);
		if (klass)
			g_warning (G_STRLOC ": %s.%s", klass->name_space, klass->name);
		g_assert_not_reached ();
	}

	return offset;
}
