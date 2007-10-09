/*
 * This is a private header file for the debugger.
 */

#ifndef __DEBUG_DEBUGGER_H__
#define __DEBUG_DEBUGGER_H__

#if !defined _IN_THE_MONO_DEBUGGER
#error "<debug-debugger.h> is a private header file only intended to be used by the debugger."
#endif

#include <mono/metadata/class-internals.h>
#include <mono/metadata/mono-debug-debugger.h>
#include "debug-mini.h"

typedef struct _MonoDebuggerInfo		MonoDebuggerInfo;
typedef struct _MonoDebuggerMetadataInfo	MonoDebuggerMetadataInfo;

/*
 * Address of the x86 trampoline code.  This is used by the debugger to check
 * whether a method is a trampoline.
 */
extern guint8 *mono_trampoline_code [];

/*
 * There's a global data symbol called `MONO_DEBUGGER__debugger_info' which
 * contains pointers to global variables and functions which must be accessed
 * by the debugger.
 */
struct _MonoDebuggerInfo {
	guint64 magic;
	guint32 version;
	guint32 total_size;
	guint32 symbol_table_size;
	guint32 dummy;
	gpointer notification_function;
	guint8 **mono_trampoline_code;
	MonoSymbolTable **symbol_table;
	MonoDebuggerMetadataInfo *metadata_info;
	guint64 (*compile_method) (guint64 method_argument);
	guint64 (*get_virtual_method) (guint64 object_argument, guint64 method_argument);
	guint64 (*get_boxed_object_method) (guint64 klass_argument, guint64 val_argument);
	MonoInvokeFunc runtime_invoke;
	guint64 (*class_get_static_field_data) (guint64 klass);
	guint64 (*run_finally) (guint64 argument1, guint64 argument2);
	void (*attach) (void);
	void (*detach) (void);
	void (*initialize) (void);
	void * (*get_lmf_addr) (void);

	guint64 (*create_string) (G_GNUC_UNUSED guint64 dummy1, G_GNUC_UNUSED guint64 dummy2,
				  const gchar *string_argument);
	gint64 (*lookup_class) (guint64 image_argument, G_GNUC_UNUSED guint64 dummy,
				gchar *full_name);
	guint64 (*insert_breakpoint) (guint64 method_argument, guint64 index);
	void (*remove_breakpoint) (G_GNUC_UNUSED guint64 dummy, guint64 index);
	void (*runtime_class_init) (guint64 klass_arg);

	gint32 *debugger_version;
	MonoDebuggerThreadInfo **thread_table;
};

struct _MonoDebuggerMetadataInfo {
	int size;
	int mono_defaults_size;
	MonoDefaults *mono_defaults;
	int type_size;
	int array_type_size;
	int klass_size;
	int thread_size;
	int thread_tid_offset;
	int thread_stack_ptr_offset;
	int thread_end_stack_offset;
	int klass_image_offset;
	int klass_instance_size_offset;
	int klass_parent_offset;
	int klass_token_offset;
	int klass_field_offset;
	int klass_methods_offset;
	int klass_method_count_offset;
	int klass_this_arg_offset;
	int klass_byval_arg_offset;
	int klass_generic_class_offset;
	int klass_generic_container_offset;
	int field_info_size;
	int field_info_type_offset;
	int field_info_offset_offset;
	int mono_defaults_corlib_offset;
	int mono_defaults_object_offset;
	int mono_defaults_byte_offset;
	int mono_defaults_void_offset;
	int mono_defaults_boolean_offset;
	int mono_defaults_sbyte_offset;
	int mono_defaults_int16_offset;
	int mono_defaults_uint16_offset;
	int mono_defaults_int32_offset;
	int mono_defaults_uint32_offset;
	int mono_defaults_int_offset;
	int mono_defaults_uint_offset;
	int mono_defaults_int64_offset;
	int mono_defaults_uint64_offset;
	int mono_defaults_single_offset;
	int mono_defaults_double_offset;
	int mono_defaults_char_offset;
	int mono_defaults_string_offset;
	int mono_defaults_enum_offset;
	int mono_defaults_array_offset;
	int mono_defaults_delegate_offset;
	int mono_defaults_exception_offset;
	int mono_method_klass_offset;
	int mono_method_token_offset;
	int mono_method_flags_offset;
	int mono_method_inflated_offset;
};

#endif
