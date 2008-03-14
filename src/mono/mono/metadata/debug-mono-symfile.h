/*
 * This header is only installed for use by the debugger:
 * the structures and the API declared here are not supported.
 */

#ifndef __MONO_DEBUG_MONO_SYMFILE_H__
#define __MONO_DEBUG_MONO_SYMFILE_H__

#include <glib.h>
#include <mono/metadata/class.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/mono-debug.h>

typedef struct MonoSymbolFileOffsetTable	MonoSymbolFileOffsetTable;
typedef struct MonoSymbolFileLineNumberEntry	MonoSymbolFileLineNumberEntry;
typedef struct MonoSymbolFileMethodEntry	MonoSymbolFileMethodEntry;
typedef struct MonoSymbolFileMethodAddress	MonoSymbolFileMethodAddress;
typedef struct MonoSymbolFileDynamicTable	MonoSymbolFileDynamicTable;
typedef struct MonoSymbolFileSourceEntry	MonoSymbolFileSourceEntry;
typedef struct MonoSymbolFileMethodIndexEntry	MonoSymbolFileMethodIndexEntry;

/* Keep in sync with OffsetTable in mcs/class/Mono.CSharp.Debugger/MonoSymbolTable.cs */
struct MonoSymbolFileOffsetTable {
	guint32 _total_file_size;
	guint32 _data_section_offset;
	guint32 _data_section_size;
	guint32 _source_count;
	guint32 _source_table_offset;
	guint32 _source_table_size;
	guint32 _method_count;
	guint32 _method_table_offset;
	guint32 _method_table_size;
	guint32 _type_count;
};

struct MonoSymbolFileMethodEntry {
	guint32 _source_index;
	guint32 _token;
	guint32 _start_row;
	guint32 _end_row;
	guint32 _num_locals;
	guint32 _num_line_numbers;
	guint32 _name_offset;
	guint32 _type_index_table_offset;
	guint32 _local_variable_table_offset;
	guint32 _line_number_table_offset;
	guint32 _num_lexical_blocks;
	guint32 _lexical_block_table_offset;
	guint32 _namespace_idx;
	guint32 _local_names_ambiguous;
};

struct MonoSymbolFileSourceEntry {
	guint32 _index;
	guint32 _num_methods;
	guint32 _num_namespaces;
	guint32 _name_offset;
	guint32 _method_offset;
	guint32 _nstable_offset;
};

struct MonoSymbolFileMethodIndexEntry {
	guint32 _file_offset;
	guint32 _token;
};

struct MonoSymbolFileMethodAddress {
	guint32 size;
	const guint8 *start_address;
	const guint8 *end_address;
	const guint8 *method_start_address;
	const guint8 *method_end_address;
	const guint8 *wrapper_address;
	guint32 has_this;
	guint32 num_params;
	guint32 variable_table_offset;
	guint32 type_table_offset;
	guint32 num_line_numbers;
	guint32 line_number_offset;
	guint8 data [MONO_ZERO_LEN_ARRAY];
};

struct MonoSymbolFileLexicalBlockEntry {
	guint32 _start_offset;
	guint32 _end_offset;
};

struct MonoSymbolFileLineNumberEntry {
	guint32 _row;
	guint32 _offset;
};

struct _MonoDebugMethodInfo {
	MonoMethod *method;
	MonoDebugHandle *handle;
	guint32 index;
	guint32 num_il_offsets;
	MonoSymbolFileLineNumberEntry *il_offsets;
	MonoSymbolFileMethodEntry *entry;
};

struct _MonoDebugLexicalBlockEntry {
	guint32 il_start_offset;
	guint32 native_start_offset;
	guint32 il_end_offset;
	guint32 native_end_offset;
};

struct _MonoDebugLineNumberEntry {
	guint32 il_offset;
	guint32 native_offset;
};

struct _MonoSymbolFile {
	const guint8 *raw_contents;
	int raw_contents_size;
	int version;
	gchar *filename;
	GHashTable *method_hash;
	MonoSymbolFileOffsetTable *offset_table;
};

#define MONO_SYMBOL_FILE_VERSION		41
#define MONO_SYMBOL_FILE_COMPATIBILITY_VERSION	39
#define MONO_SYMBOL_FILE_MAGIC			0x45e82623fd7fa614ULL

G_BEGIN_DECLS

MonoSymbolFile *
mono_debug_open_mono_symbols       (MonoDebugHandle          *handle,
				    const guint8             *raw_contents,
				    int                       size,
				    gboolean                  in_the_debugger);

void
mono_debug_close_mono_symbol_file  (MonoSymbolFile           *symfile);

MonoDebugSourceLocation *
mono_debug_symfile_lookup_location (MonoDebugMethodInfo      *minfo,
				    guint32                   offset);

gint32
_mono_debug_address_from_il_offset (MonoDebugMethodJitInfo   *jit,
				    guint32                   il_offset);

MonoDebugMethodInfo *
mono_debug_symfile_lookup_method   (MonoDebugHandle          *handle,
				    MonoMethod               *method);
G_END_DECLS

#endif /* __MONO_SYMFILE_H__ */

