
/*
 * reflection.c: Routines for creating an image at runtime.
 * 
 * Author:
 *   Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2001, 2002 Ximian, Inc.  http://www.ximian.com
 *
 */
#include <config.h>
#include "mono/utils/mono-digest.h"
#include "mono/metadata/reflection.h"
#include "mono/metadata/tabledefs.h"
#include "mono/metadata/tokentype.h"
#include "mono/metadata/appdomain.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/assembly.h"
#include <mono/metadata/exception.h>
#include <stdio.h>
#include <glib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include "image.h"
#include "cil-coff.h"
#include "rawbuffer.h"
#include "mono-endian.h"
#include "private.h"
#include <mono/os/gc_wrapper.h>

#define TEXT_OFFSET 512
#define CLI_H_SIZE 136
#define FILE_ALIGN 512
#define VIRT_ALIGN 8192
#define START_TEXT_RVA  0x00002000

typedef struct {
	MonoReflectionILGen *ilgen;
	MonoReflectionType *rtype;
	MonoArray *parameters;
	MonoArray *generic_params;
	MonoArray *pinfo;
	guint32 attrs;
	guint32 iattrs;
	guint32 call_conv;
	guint32 *table_idx; /* note: it's a pointer */
	MonoArray *code;
	MonoObject *type;
	MonoString *name;
	MonoBoolean init_locals;
	MonoMethod *mhandle;
} ReflectionMethodBuilder;

const unsigned char table_sizes [64] = {
	MONO_MODULE_SIZE,
	MONO_TYPEREF_SIZE,
	MONO_TYPEDEF_SIZE,
	0,
	MONO_FIELD_SIZE,
	0,
	MONO_METHOD_SIZE,
	0,
	MONO_PARAM_SIZE,
	MONO_INTERFACEIMPL_SIZE,
	MONO_MEMBERREF_SIZE,	/* 0x0A */
	MONO_CONSTANT_SIZE,
	MONO_CUSTOM_ATTR_SIZE,
	MONO_FIELD_MARSHAL_SIZE,
	MONO_DECL_SECURITY_SIZE,
	MONO_CLASS_LAYOUT_SIZE,
	MONO_FIELD_LAYOUT_SIZE,	/* 0x10 */
	MONO_STAND_ALONE_SIGNATURE_SIZE,
	MONO_EVENT_MAP_SIZE,
	0,
	MONO_EVENT_SIZE,
	MONO_PROPERTY_MAP_SIZE,
	0,
	MONO_PROPERTY_SIZE,
	MONO_METHOD_SEMA_SIZE,
	MONO_METHODIMPL_SIZE,
	MONO_MODULEREF_SIZE,	/* 0x1A */
	MONO_TYPESPEC_SIZE,
	MONO_IMPLMAP_SIZE,	
	MONO_FIELD_RVA_SIZE,
	0,
	0,
	MONO_ASSEMBLY_SIZE,	/* 0x20 */
	MONO_ASSEMBLY_PROCESSOR_SIZE,
	MONO_ASSEMBLYOS_SIZE,
	MONO_ASSEMBLYREF_SIZE,
	MONO_ASSEMBLYREFPROC_SIZE,
	MONO_ASSEMBLYREFOS_SIZE,
	MONO_FILE_SIZE,
	MONO_EXP_TYPE_SIZE,
	MONO_MANIFEST_SIZE,
	MONO_NESTED_CLASS_SIZE,

	MONO_GENERICPARAM_SIZE,	/* 0x2A */
	MONO_METHODSPEC_SIZE,
	MONO_GENPARCONSTRAINT_SIZE,

	0	/* 0x2D */
};

/**
 * These macros can be used to allocate long living atomic data so it won't be
 * tracked by the garbage collector. We use libgc because it's apparently faster
 * than g_malloc.
 */
#ifdef HAVE_BOEHM_GC
#define ALLOC_ATOMIC(size) GC_MALLOC_ATOMIC (size)
#define FREE_ATOMIC(ptr)
#define REALLOC_ATOMIC(ptr, size) GC_REALLOC ((ptr), (size))
#else
#define ALLOC_ATOMIC(size) g_malloc (size)
#define FREE_ATOMIC(ptr) g_free (ptr)
#define REALLOC_ATOMIC(ptr, size) g_realloc ((ptr), (size))
#endif

static void reflection_methodbuilder_from_method_builder (ReflectionMethodBuilder *rmb, MonoReflectionMethodBuilder *mb);
static void reflection_methodbuilder_from_ctor_builder (ReflectionMethodBuilder *rmb, MonoReflectionCtorBuilder *mb);
static guint32 mono_image_typedef_or_ref (MonoDynamicImage *assembly, MonoType *type);
static guint32 mono_image_get_methodref_token (MonoDynamicImage *assembly, MonoMethod *method);
static guint32 mono_image_get_methodbuilder_token (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *mb);
static guint32 mono_image_get_ctorbuilder_token (MonoDynamicImage *assembly, MonoReflectionCtorBuilder *cb);
static guint32 mono_image_get_sighelper_token (MonoDynamicImage *assembly, MonoReflectionSigHelper *helper);
static void    mono_image_get_generic_param_info (MonoReflectionGenericParam *gparam, guint32 owner, MonoDynamicImage *assembly);
static guint32 encode_marshal_blob (MonoDynamicImage *assembly, MonoReflectionMarshal *minfo);
static char*   type_get_qualified_name (MonoType *type, MonoAssembly *ass);
static void    ensure_runtime_vtable (MonoClass *klass);

static void
alloc_table (MonoDynamicTable *table, guint nrows)
{
	table->rows = nrows;
	g_assert (table->columns);
	if (nrows + 1 >= table->alloc_rows) {
		while (nrows + 1 >= table->alloc_rows)
			if (table->alloc_rows == 0)
				table->alloc_rows = 16;
			else
				table->alloc_rows *= 2;

		if (table->values)
			table->values = REALLOC_ATOMIC (table->values, (table->alloc_rows) * table->columns * sizeof (guint32));
		else
			table->values = ALLOC_ATOMIC ((table->alloc_rows) * table->columns * sizeof (guint32));
	}
}

static void
make_room_in_stream (MonoDynamicStream *stream, int size)
{
	while (stream->alloc_size <= size) {
		if (stream->alloc_size < 4096)
			stream->alloc_size = 4096;
		else
			stream->alloc_size *= 2;
	}
	if (stream->data)
		stream->data = REALLOC_ATOMIC (stream->data, stream->alloc_size);
	else
		stream->data = ALLOC_ATOMIC (stream->alloc_size);
}	

static guint32
string_heap_insert (MonoDynamicStream *sh, const char *str)
{
	guint32 idx;
	guint32 len;
	gpointer oldkey, oldval;

	if (g_hash_table_lookup_extended (sh->hash, str, &oldkey, &oldval))
		return GPOINTER_TO_UINT (oldval);

	len = strlen (str) + 1;
	idx = sh->index;
	if (idx + len > sh->alloc_size)
		make_room_in_stream (sh, idx + len);

	/*
	 * We strdup the string even if we already copy them in sh->data
	 * so that the string pointers in the hash remain valid even if
	 * we need to realloc sh->data. We may want to avoid that later.
	 */
	g_hash_table_insert (sh->hash, g_strdup (str), GUINT_TO_POINTER (idx));
	memcpy (sh->data + idx, str, len);
	sh->index += len;
	return idx;
}

static void
string_heap_init (MonoDynamicStream *sh)
{
	sh->index = 0;
	sh->alloc_size = 4096;
	sh->data = ALLOC_ATOMIC (4096);
	sh->hash = g_hash_table_new (g_str_hash, g_str_equal);
	string_heap_insert (sh, "");
}

#if 0 /* never used */
static void
string_heap_free (MonoDynamicStream *sh)
{
	FREE_ATOMIC (sh->data);
	g_hash_table_foreach (sh->hash, (GHFunc)g_free, NULL);
	g_hash_table_destroy (sh->hash);
}
#endif

static guint32
mono_image_add_stream_data (MonoDynamicStream *stream, const char *data, guint32 len)
{
	guint32 idx;
	if (stream->alloc_size < stream->index + len)
		make_room_in_stream (stream, stream->index + len);
	memcpy (stream->data + stream->index, data, len);
	idx = stream->index;
	stream->index += len;
	/* 
	 * align index? Not without adding an additional param that controls it since
	 * we may store a blob value in pieces.
	 */
	return idx;
}

static guint32
mono_image_add_stream_zero (MonoDynamicStream *stream, guint32 len)
{
	guint32 idx;
	if (stream->alloc_size < stream->index + len)
		make_room_in_stream (stream, stream->index + len);
	memset (stream->data + stream->index, 0, len);
	idx = stream->index;
	stream->index += len;
	return idx;
}

static void
stream_data_align (MonoDynamicStream *stream)
{
	char buf [4] = {0};
	guint32 count = stream->index % 4;

	/* we assume the stream data will be aligned */
	if (count)
		mono_image_add_stream_data (stream, buf, 4 - count);
}

static int
mono_blob_entry_hash (const char* str)
{
	guint len, h;
	const char *end;
	len = mono_metadata_decode_blob_size (str, &str);
	if (len > 0) {
		end = str + len;
		h = *str;
		for (str += 1; str < end; str++)
			h = (h << 5) - h + *str;
		return h;
	}
	else
		return 0;
}

static gboolean
mono_blob_entry_equal (const char *str1, const char *str2) {
	int len, len2;
	const char *end1;
	const char *end2;
	len = mono_metadata_decode_blob_size (str1, &end1);
	len2 = mono_metadata_decode_blob_size (str2, &end2);
	if (len != len2)
		return 0;
	return memcmp (end1, end2, len) == 0;
}

static guint32
add_to_blob_cached (MonoDynamicImage *assembly, char *b1, int s1, char *b2, int s2)
{
	guint32 idx;
	char *copy;
	gpointer oldkey, oldval;
	
	copy = ALLOC_ATOMIC (s1+s2);
	memcpy (copy, b1, s1);
	memcpy (copy + s1, b2, s2);
	if (mono_g_hash_table_lookup_extended (assembly->blob_cache, copy, &oldkey, &oldval)) {
		FREE_ATOMIC (copy);
		idx = GPOINTER_TO_UINT (oldval);
	} else {
		idx = mono_image_add_stream_data (&assembly->blob, b1, s1);
		mono_image_add_stream_data (&assembly->blob, b2, s2);
		mono_g_hash_table_insert (assembly->blob_cache, copy, GUINT_TO_POINTER (idx));
	}
	return idx;
}

/* modified version needed to handle building corlib */
static MonoClass*
my_mono_class_from_mono_type (MonoType *type) {
	switch (type->type) {
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_PTR:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_GENERICINST:
		return mono_class_from_mono_type (type);
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		g_assert (type->data.generic_param->pklass);
		return type->data.generic_param->pklass;
	default:
		/* should be always valid when we reach this case... */
		return type->data.klass;
	}
}

static void
encode_type (MonoDynamicImage *assembly, MonoType *type, char *p, char **endbuf)
{
	if (!type) {
		g_assert_not_reached ();
		return;
	}
		
	if (type->byref)
		mono_metadata_encode_value (MONO_TYPE_BYREF, p, &p);

	switch (type->type){
	case MONO_TYPE_VOID:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_STRING:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_TYPEDBYREF:
		mono_metadata_encode_value (type->type, p, &p);
		break;
	case MONO_TYPE_PTR:
		mono_metadata_encode_value (type->type, p, &p);
		encode_type (assembly, type->data.type, p, &p);
		break;
	case MONO_TYPE_SZARRAY:
		mono_metadata_encode_value (type->type, p, &p);
		encode_type (assembly, &type->data.klass->byval_arg, p, &p);
		break;

	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS: {
		MonoClass *k = mono_class_from_mono_type (type);
		mono_metadata_encode_value (type->type, p, &p);
		/*
		 * ensure only non-byref gets passed to mono_image_typedef_or_ref(),
		 * otherwise two typerefs could point to the same type, leading to
		 * verification errors.
		 */
		mono_metadata_encode_value (mono_image_typedef_or_ref (assembly, &k->byval_arg), p, &p);
		break;
	}

	case MONO_TYPE_ARRAY:
		mono_metadata_encode_value (type->type, p, &p);
		encode_type (assembly, &type->data.array->eklass->byval_arg, p, &p);
		mono_metadata_encode_value (type->data.array->rank, p, &p);
		mono_metadata_encode_value (0, p, &p); /* FIXME: set to 0 for now */
		mono_metadata_encode_value (0, p, &p);
		break;
	case MONO_TYPE_GENERICINST: {
		int i;
		mono_metadata_encode_value (type->type, p, &p);
		encode_type (assembly, type->data.generic_inst->generic_type, p, &p);
		mono_metadata_encode_value (type->data.generic_inst->type_argc, p, &p);
		for (i = 0; i < type->data.generic_inst->type_argc; ++i) {
			encode_type (assembly, type->data.generic_inst->type_argv [i], p, &p);
		}
		break;
	}
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		mono_metadata_encode_value (type->type, p, &p);
		mono_metadata_encode_value (type->data.generic_param->num, p, &p);
		break;

	default:
		g_error ("need to encode type %x", type->type);
	}
	*endbuf = p;
}

static void
encode_reflection_type (MonoDynamicImage *assembly, MonoReflectionType *type, char *p, char **endbuf)
{
	if (!type) {
		mono_metadata_encode_value (MONO_TYPE_VOID, p, endbuf);
		return;
	}
	if (type->type) {
		encode_type (assembly, type->type, p, endbuf);
		return;
	}

	g_assert_not_reached ();

}

static guint32
method_encode_signature (MonoDynamicImage *assembly, MonoMethodSignature *sig)
{
	char *buf;
	char *p;
	int i;
	guint32 nparams =  sig->param_count;
	guint32 size = 10 + nparams * 10;
	guint32 idx;
	char blob_size [6];
	char *b = blob_size;

	if (!assembly->save)
		return 0;

	p = buf = g_malloc (size);
	/*
	 * FIXME: vararg, explicit_this, differenc call_conv values...
	 */
	*p = sig->call_convention;
	if (sig->hasthis)
		*p |= 0x20; /* hasthis */
	if (sig->generic_param_count)
		*p |= 0x10; /* generic */
	p++;
	if (sig->generic_param_count)
		mono_metadata_encode_value (sig->generic_param_count, p, &p);
	mono_metadata_encode_value (nparams, p, &p);
	encode_type (assembly, sig->ret, p, &p);
	for (i = 0; i < nparams; ++i)
		encode_type (assembly, sig->params [i], p, &p);
	/* store length */
	g_assert (p - buf < size);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);
	return idx;
}

static guint32
method_builder_encode_signature (MonoDynamicImage *assembly, ReflectionMethodBuilder *mb)
{
	/*
	 * FIXME: reuse code from method_encode_signature().
	 */
	char *buf;
	char *p;
	int i;
	guint32 nparams =  mb->parameters ? mono_array_length (mb->parameters): 0;
	guint32 ngparams = mb->generic_params ? mono_array_length (mb->generic_params): 0;
	guint32 size = 11 + nparams * 10;
	guint32 idx;
	char blob_size [6];
	char *b = blob_size;

	p = buf = g_malloc (size);
	/* LAMESPEC: all the call conv spec is foobared */
	*p = mb->call_conv & 0x60; /* has-this, explicit-this */
	if (mb->call_conv & 2)
		*p |= 0x5; /* vararg */
	if (!(mb->attrs & METHOD_ATTRIBUTE_STATIC))
		*p |= 0x20; /* hasthis */
	if (ngparams)
		*p |= 0x10; /* generic */
	p++;
	if (ngparams)
		mono_metadata_encode_value (ngparams, p, &p);
	mono_metadata_encode_value (nparams, p, &p);
	encode_reflection_type (assembly, mb->rtype, p, &p);
	for (i = 0; i < nparams; ++i) {
		MonoReflectionType *pt = mono_array_get (mb->parameters, MonoReflectionType*, i);
		encode_reflection_type (assembly, pt, p, &p);
	}
	/* store length */
	g_assert (p - buf < size);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);
	return idx;
}

static guint32
encode_locals (MonoDynamicImage *assembly, MonoReflectionILGen *ilgen)
{
	MonoDynamicTable *table;
	guint32 *values;
	char *p;
	guint32 idx, sig_idx, size;
	guint nl = mono_array_length (ilgen->locals);
	char *buf;
	char blob_size [6];
	char *b = blob_size;
	int i;

	size = 10 + nl * 10;
	p = buf = g_malloc (size);
	table = &assembly->tables [MONO_TABLE_STANDALONESIG];
	idx = table->next_idx ++;
	table->rows ++;
	alloc_table (table, table->rows);
	values = table->values + idx * MONO_STAND_ALONE_SIGNATURE_SIZE;

	mono_metadata_encode_value (0x07, p, &p);
	mono_metadata_encode_value (nl, p, &p);
	for (i = 0; i < nl; ++i) {
		MonoReflectionLocalBuilder *lb = mono_array_get (ilgen->locals, MonoReflectionLocalBuilder*, i);
		encode_reflection_type (assembly, lb->type, p, &p);
	}
	g_assert (p - buf < size);
	mono_metadata_encode_value (p-buf, b, &b);
	sig_idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);

	values [MONO_STAND_ALONE_SIGNATURE] = sig_idx;

	return idx;
}

static guint32
method_count_clauses (MonoReflectionILGen *ilgen)
{
	guint32 num_clauses = 0;
	int i;

	MonoILExceptionInfo *ex_info;
	for (i = 0; i < mono_array_length (ilgen->ex_handlers); ++i) {
		ex_info = (MonoILExceptionInfo*)mono_array_addr (ilgen->ex_handlers, MonoILExceptionInfo, i);
		if (ex_info->handlers)
			num_clauses += mono_array_length (ex_info->handlers);
		else
			num_clauses++;
	}

	return num_clauses;
}

static MonoExceptionClause*
method_encode_clauses (MonoDynamicImage *assembly,
					   MonoReflectionILGen *ilgen, guint32 num_clauses)
{
	MonoExceptionClause *clauses;
	MonoExceptionClause *clause;
	MonoILExceptionInfo *ex_info;
	MonoILExceptionBlock *ex_block;
	guint32 finally_start;
	int i, j, clause_index;;

	clauses = g_new0 (MonoExceptionClause, num_clauses);

	clause_index = 0;
	for (i = mono_array_length (ilgen->ex_handlers) - 1; i >= 0; --i) {
		ex_info = (MonoILExceptionInfo*)mono_array_addr (ilgen->ex_handlers, MonoILExceptionInfo, i);
		finally_start = ex_info->start + ex_info->len;
		g_assert (ex_info->handlers);
		for (j = 0; j < mono_array_length (ex_info->handlers); ++j) {
			ex_block = (MonoILExceptionBlock*)mono_array_addr (ex_info->handlers, MonoILExceptionBlock, j);
			clause = &(clauses [clause_index]);

			clause->flags = ex_block->type;
			clause->try_offset = ex_info->start;

			if (ex_block->type == MONO_EXCEPTION_CLAUSE_FINALLY)
				clause->try_len = finally_start - ex_info->start;
			else
				clause->try_len = ex_info->len;
			clause->handler_offset = ex_block->start;
			clause->handler_len = ex_block->len;
			clause->token_or_filter = ex_block->extype ? mono_metadata_token_from_dor (
				mono_image_typedef_or_ref (assembly, ex_block->extype->type)): 0;
			if (ex_block->extype) {
				mono_g_hash_table_insert (assembly->tokens,
										  GUINT_TO_POINTER (clause->token_or_filter),
										  ex_block->extype);
			}
			finally_start = ex_block->start + ex_block->len;

			clause_index ++;
		}
	}

	return clauses;
}

static guint32
method_encode_code (MonoDynamicImage *assembly, ReflectionMethodBuilder *mb)
{
	char flags = 0;
	guint32 idx;
	guint32 code_size;
	gint32 max_stack, i;
	gint32 num_locals = 0;
	gint32 num_exception = 0;
	gint maybe_small;
	guint32 fat_flags;
	char fat_header [12];
	guint32 *intp;
	guint16 *shortp;
	guint32 local_sig = 0;
	guint32 header_size = 12;
	MonoArray *code;

	if ((mb->attrs & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
			(mb->iattrs & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) ||
			(mb->iattrs & METHOD_IMPL_ATTRIBUTE_RUNTIME) ||
			(mb->attrs & METHOD_ATTRIBUTE_ABSTRACT))
		return 0;

	/*if (mb->name)
		g_print ("Encode method %s\n", mono_string_to_utf8 (mb->name));*/
	if (mb->ilgen) {
		code = mb->ilgen->code;
		code_size = mb->ilgen->code_len;
		max_stack = mb->ilgen->max_stack;
		num_locals = mb->ilgen->locals ? mono_array_length (mb->ilgen->locals) : 0;
		if (mb->ilgen->ex_handlers)
			num_exception = method_count_clauses (mb->ilgen);
	} else {
		code = mb->code;
		if (code == NULL)
			mono_raise_exception (mono_get_exception_argument (NULL, "a method does not have any IL associated"));

		code_size = mono_array_length (code);
		max_stack = 8; /* we probably need to run a verifier on the code... */
	}

	/* check for exceptions, maxstack, locals */
	maybe_small = (max_stack <= 8) && (!num_locals) && (!num_exception);
	if (maybe_small) {
		if (code_size < 64 && !(code_size & 1)) {
			flags = (code_size << 2) | 0x2;
		} else if (code_size < 32 && (code_size & 1)) {
			flags = (code_size << 2) | 0x6; /* LAMESPEC: see metadata.c */
		} else {
			goto fat_header;
		}
		idx = mono_image_add_stream_data (&assembly->code, &flags, 1);
		/* add to the fixup todo list */
		if (mb->ilgen && mb->ilgen->num_token_fixups)
			mono_g_hash_table_insert (assembly->token_fixups, mb->ilgen, GUINT_TO_POINTER (idx + 1));
		mono_image_add_stream_data (&assembly->code, mono_array_addr (code, char, 0), code_size);
		return assembly->text_rva + idx;
	} 
fat_header:
	if (num_locals)
		local_sig = MONO_TOKEN_SIGNATURE | encode_locals (assembly, mb->ilgen);
	/* 
	 * FIXME: need to set also the header size in fat_flags.
	 * (and more sects and init locals flags)
	 */
	fat_flags =  0x03;
	if (num_exception)
		fat_flags |= METHOD_HEADER_MORE_SECTS;
	if (mb->init_locals)
		fat_flags |= METHOD_HEADER_INIT_LOCALS;
	fat_header [0] = fat_flags;
	fat_header [1] = (header_size / 4 ) << 4;
	shortp = (guint16*)(fat_header + 2);
	*shortp = GUINT16_TO_LE (max_stack);
	intp = (guint32*)(fat_header + 4);
	*intp = GUINT32_TO_LE (code_size);
	intp = (guint32*)(fat_header + 8);
	*intp = GUINT32_TO_LE (local_sig);
	idx = mono_image_add_stream_data (&assembly->code, fat_header, 12);
	/* add to the fixup todo list */
	if (mb->ilgen && mb->ilgen->num_token_fixups)
		mono_g_hash_table_insert (assembly->token_fixups, mb->ilgen, GUINT_TO_POINTER (idx + 12));
	
	mono_image_add_stream_data (&assembly->code, mono_array_addr (code, char, 0), code_size);
	if (num_exception) {
		unsigned char sheader [4];
		MonoExceptionClause clause;
		MonoILExceptionInfo * ex_info;
		MonoILExceptionBlock * ex_block;
		int j;

		stream_data_align (&assembly->code);
		/* always use fat format for now */
		sheader [0] = METHOD_HEADER_SECTION_FAT_FORMAT | METHOD_HEADER_SECTION_EHTABLE;
		num_exception *= sizeof (MonoExceptionClause);
		num_exception += 4; /* include the size of the header */
		sheader [1] = num_exception & 0xff;
		sheader [2] = (num_exception >> 8) & 0xff;
		sheader [3] = (num_exception >> 16) & 0xff;
		mono_image_add_stream_data (&assembly->code, sheader, 4);
		/* fat header, so we are already aligned */
		/* reverse order */
		for (i = mono_array_length (mb->ilgen->ex_handlers) - 1; i >= 0; --i) {
			ex_info = (MonoILExceptionInfo *)mono_array_addr (mb->ilgen->ex_handlers, MonoILExceptionInfo, i);
			if (ex_info->handlers) {
				int finally_start = ex_info->start + ex_info->len;
				for (j = 0; j < mono_array_length (ex_info->handlers); ++j) {
					ex_block = (MonoILExceptionBlock*)mono_array_addr (ex_info->handlers, MonoILExceptionBlock, j);
					clause.flags = GUINT32_TO_LE (ex_block->type);
					clause.try_offset = GUINT32_TO_LE (ex_info->start);
					/* need fault, too, probably */
					if (ex_block->type == MONO_EXCEPTION_CLAUSE_FINALLY)
						clause.try_len = GUINT32_TO_LE (finally_start - ex_info->start);
					else
						clause.try_len = GUINT32_TO_LE (ex_info->len);
					clause.handler_offset = GUINT32_TO_LE (ex_block->start);
					clause.handler_len = GUINT32_TO_LE (ex_block->len);
					finally_start = ex_block->start + ex_block->len;
					clause.token_or_filter = ex_block->extype ? mono_metadata_token_from_dor (
							mono_image_typedef_or_ref (assembly, ex_block->extype->type)): 0;
					clause.token_or_filter = GUINT32_TO_LE (clause.token_or_filter);
					/*g_print ("out clause %d: from %d len=%d, handler at %d, %d, finally_start=%d, ex_info->start=%d, ex_info->len=%d, ex_block->type=%d, j=%d, i=%d\n", 
							clause.flags, clause.try_offset, clause.try_len, clause.handler_offset, clause.handler_len, finally_start, ex_info->start, ex_info->len, ex_block->type, j, i);*/
					mono_image_add_stream_data (&assembly->code, (char*)&clause, sizeof (clause));
				}
			} else {
				g_error ("No clauses for ex info block %d", i);
			}
		}
	}
	return assembly->text_rva + idx;
}

static guint32
find_index_in_table (MonoDynamicImage *assembly, int table_idx, int col, guint32 token)
{
	int i;
	MonoDynamicTable *table;
	guint32 *values;
	
	table = &assembly->tables [table_idx];

	g_assert (col < table->columns);

	values = table->values + table->columns;
	for (i = 1; i <= table->rows; ++i) {
		if (values [col] == token)
			return i;
		values += table->columns;
	}
	return 0;
}

static GHashTable *dynamic_custom_attrs = NULL;

static MonoCustomAttrInfo*
mono_custom_attrs_from_builders (MonoImage *image, MonoArray *cattrs)
{
	int i, count;
	MonoCustomAttrInfo *ainfo;
	MonoReflectionCustomAttr *cattr;

	if (!cattrs)
		return NULL;
	/* FIXME: check in assembly the Run flag is set */

	count = mono_array_length (cattrs);

	ainfo = g_malloc0 (sizeof (MonoCustomAttrInfo) + sizeof (MonoCustomAttrEntry) * (count - MONO_ZERO_LEN_ARRAY));

	ainfo->image = image;
	ainfo->num_attrs = count;
	for (i = 0; i < count; ++i) {
		cattr = (MonoReflectionCustomAttr*)mono_array_get (cattrs, gpointer, i);
		ainfo->attrs [i].ctor = cattr->ctor->method;
		/* FIXME: might want to memdup the data here */
		ainfo->attrs [i].data = mono_array_addr (cattr->data, char, 0);
		ainfo->attrs [i].data_size = mono_array_length (cattr->data);
	}

	return ainfo;
}

static void
mono_save_custom_attrs (MonoImage *image, void *obj, MonoArray *cattrs)
{
	MonoCustomAttrInfo *ainfo = mono_custom_attrs_from_builders (image, cattrs);

	if (!ainfo)
		return;

	if (!dynamic_custom_attrs)
		dynamic_custom_attrs = g_hash_table_new (NULL, NULL);

	g_hash_table_insert (dynamic_custom_attrs, obj, ainfo);
}

void
mono_custom_attrs_free (MonoCustomAttrInfo *ainfo)
{
	/* they are cached, so we don't free them */
	if (dynamic_custom_attrs && g_hash_table_lookup (dynamic_custom_attrs, ainfo))
		return;
	g_free (ainfo);
}

/*
 * idx is the table index of the object
 * type is one of CUSTOM_ATTR_*
 */
static void
mono_image_add_cattrs (MonoDynamicImage *assembly, guint32 idx, guint32 type, MonoArray *cattrs)
{
	MonoDynamicTable *table;
	MonoReflectionCustomAttr *cattr;
	guint32 *values;
	guint32 count, i, token;
	char blob_size [6];
	char *p = blob_size;
	
	/* it is legal to pass a NULL cattrs: we avoid to use the if in a lot of places */
	if (!cattrs)
		return;
	count = mono_array_length (cattrs);
	table = &assembly->tables [MONO_TABLE_CUSTOMATTRIBUTE];
	table->rows += count;
	alloc_table (table, table->rows);
	values = table->values + table->next_idx * MONO_CUSTOM_ATTR_SIZE;
	idx <<= CUSTOM_ATTR_BITS;
	idx |= type;
	for (i = 0; i < count; ++i) {
		cattr = (MonoReflectionCustomAttr*)mono_array_get (cattrs, gpointer, i);
		values [MONO_CUSTOM_ATTR_PARENT] = idx;
		token = mono_image_create_token (assembly, (MonoObject*)cattr->ctor);
		type = mono_metadata_token_index (token);
		type <<= CUSTOM_ATTR_TYPE_BITS;
		switch (mono_metadata_token_table (token)) {
		case MONO_TABLE_METHOD:
			type |= CUSTOM_ATTR_TYPE_METHODDEF;
			break;
		case MONO_TABLE_MEMBERREF:
			type |= CUSTOM_ATTR_TYPE_MEMBERREF;
			break;
		default:
			g_warning ("got wrong token in custom attr");
			continue;
		}
		values [MONO_CUSTOM_ATTR_TYPE] = type;
		p = blob_size;
		mono_metadata_encode_value (mono_array_length (cattr->data), p, &p);
		values [MONO_CUSTOM_ATTR_VALUE] = add_to_blob_cached (assembly, blob_size, p - blob_size,
			mono_array_addr (cattr->data, char, 0), mono_array_length (cattr->data));
		values += MONO_CUSTOM_ATTR_SIZE;
		++table->next_idx;
	}
}

/*
 * Fill in the MethodDef and ParamDef tables for a method.
 * This is used for both normal methods and constructors.
 */
static void
mono_image_basic_method (ReflectionMethodBuilder *mb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 *values;
	char *name;
	guint i, count;

	/* room in this table is already allocated */
	table = &assembly->tables [MONO_TABLE_METHOD];
	*mb->table_idx = table->next_idx ++;
	mono_g_hash_table_insert (assembly->method_to_table_idx, mb->mhandle, GUINT_TO_POINTER ((*mb->table_idx)));
	values = table->values + *mb->table_idx * MONO_METHOD_SIZE;
	name = mono_string_to_utf8 (mb->name);
	values [MONO_METHOD_NAME] = string_heap_insert (&assembly->sheap, name);
	g_free (name);
	values [MONO_METHOD_FLAGS] = mb->attrs;
	values [MONO_METHOD_IMPLFLAGS] = mb->iattrs;
	values [MONO_METHOD_SIGNATURE] = method_builder_encode_signature (assembly, mb);
	values [MONO_METHOD_RVA] = method_encode_code (assembly, mb);
	
	table = &assembly->tables [MONO_TABLE_PARAM];
	values [MONO_METHOD_PARAMLIST] = table->next_idx;

	if (mb->pinfo) {
		MonoDynamicTable *mtable;
		guint32 *mvalues;
		
		mtable = &assembly->tables [MONO_TABLE_FIELDMARSHAL];
		mvalues = mtable->values + mtable->next_idx * MONO_FIELD_MARSHAL_SIZE;
		
		count = 0;
		for (i = 0; i < mono_array_length (mb->pinfo); ++i) {
			if (mono_array_get (mb->pinfo, gpointer, i))
				count++;
		}
		table->rows += count;
		alloc_table (table, table->rows);
		values = table->values + table->next_idx * MONO_PARAM_SIZE;
		for (i = 0; i < mono_array_length (mb->pinfo); ++i) {
			MonoReflectionParamBuilder *pb;
			if ((pb = mono_array_get (mb->pinfo, MonoReflectionParamBuilder*, i))) {
				values [MONO_PARAM_FLAGS] = pb->attrs;
				values [MONO_PARAM_SEQUENCE] = i;
				if (pb->name != NULL) {
					name = mono_string_to_utf8 (pb->name);
					values [MONO_PARAM_NAME] = string_heap_insert (&assembly->sheap, name);
					g_free (name);
				}
				else
					values [MONO_PARAM_NAME] = 0;
				values += MONO_PARAM_SIZE;
				if (pb->marshal_info) {
					mtable->rows++;
					alloc_table (mtable, mtable->rows);
					mvalues = mtable->values + mtable->rows * MONO_FIELD_MARSHAL_SIZE;
					mvalues [MONO_FIELD_MARSHAL_PARENT] = (table->next_idx << HAS_FIELD_MARSHAL_BITS) | HAS_FIELD_MARSHAL_PARAMDEF;
					mvalues [MONO_FIELD_MARSHAL_NATIVE_TYPE] = encode_marshal_blob (assembly, pb->marshal_info);
				}
				pb->table_idx = table->next_idx++;
			}
		}
	}
}

static void
reflection_methodbuilder_from_method_builder (ReflectionMethodBuilder *rmb,
											   MonoReflectionMethodBuilder *mb)
{
	rmb->ilgen = mb->ilgen;
	rmb->rtype = mb->rtype;
	rmb->parameters = mb->parameters;
	rmb->generic_params = mb->generic_params;
	rmb->pinfo = mb->pinfo;
	rmb->attrs = mb->attrs;
	rmb->iattrs = mb->iattrs;
	rmb->call_conv = mb->call_conv;
	rmb->code = mb->code;
	rmb->type = mb->type;
	rmb->name = mb->name;
	rmb->table_idx = &mb->table_idx;
	rmb->init_locals = mb->init_locals;
	rmb->mhandle = mb->mhandle;
}

static void
reflection_methodbuilder_from_ctor_builder (ReflectionMethodBuilder *rmb,
											 MonoReflectionCtorBuilder *mb)
{
	const char *name = mb->attrs & METHOD_ATTRIBUTE_STATIC ? ".cctor": ".ctor";

	rmb->ilgen = mb->ilgen;
	rmb->rtype = mono_type_get_object (mono_domain_get (), &mono_defaults.void_class->byval_arg);
	rmb->parameters = mb->parameters;
	rmb->generic_params = NULL;
	rmb->pinfo = mb->pinfo;
	rmb->attrs = mb->attrs;
	rmb->iattrs = mb->iattrs;
	rmb->call_conv = mb->call_conv;
	rmb->code = NULL;
	rmb->type = mb->type;
	rmb->name = mono_string_new (mono_domain_get (), name);
	rmb->table_idx = &mb->table_idx;
	rmb->init_locals = mb->init_locals;
	rmb->mhandle = mb->mhandle;
}

static void
mono_image_get_method_info (MonoReflectionMethodBuilder *mb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 *values;
	char *name;
	ReflectionMethodBuilder rmb;
	int i;

	reflection_methodbuilder_from_method_builder (&rmb, mb);

	mono_image_basic_method (&rmb, assembly);

	if (mb->dll) { /* It's a P/Invoke method */
		guint32 moduleref;
		int charset = mb->charset & 0xf;
		int lasterr = mb->charset & 0x40;
		table = &assembly->tables [MONO_TABLE_IMPLMAP];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_IMPLMAP_SIZE;
		/* map CharSet values to on-disk values */
		
		values [MONO_IMPLMAP_FLAGS] = (mb->native_cc << 8) | (charset ? (charset - 1) * 2: 1) | lasterr;
		values [MONO_IMPLMAP_MEMBER] = (mb->table_idx << 1) | 1; /* memberforwarded: method */
		name = mono_string_to_utf8 (mb->dllentry);
		values [MONO_IMPLMAP_NAME] = string_heap_insert (&assembly->sheap, name);
		g_free (name);
		name = mono_string_to_utf8 (mb->dll);
		moduleref = string_heap_insert (&assembly->sheap, name);
		g_free (name);
		if (!(values [MONO_IMPLMAP_SCOPE] = find_index_in_table (assembly, MONO_TABLE_MODULEREF, MONO_MODULEREF_NAME, moduleref))) {
			table = &assembly->tables [MONO_TABLE_MODULEREF];
			table->rows ++;
			alloc_table (table, table->rows);
			table->values [table->rows * MONO_MODULEREF_SIZE + MONO_MODULEREF_NAME] = moduleref;
			values [MONO_IMPLMAP_SCOPE] = table->rows;
		}
	}

	if (mb->override_method) {
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mb->type;
		guint32 tok;
		table = &assembly->tables [MONO_TABLE_METHODIMPL];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_METHODIMPL_SIZE;
		values [MONO_METHODIMPL_CLASS] = tb->table_idx;
		values [MONO_METHODIMPL_BODY] = METHODDEFORREF_METHODDEF | (mb->table_idx << METHODDEFORREF_BITS);
		tok = mono_image_create_token (assembly, (MonoObject*)mb->override_method);
		switch (mono_metadata_token_table (tok)) {
		case MONO_TABLE_MEMBERREF:
			tok = (mono_metadata_token_index (tok) << METHODDEFORREF_BITS ) | METHODDEFORREF_METHODREF;
			break;
		case MONO_TABLE_METHOD:
			tok = (mono_metadata_token_index (tok) << METHODDEFORREF_BITS ) | METHODDEFORREF_METHODDEF;
			break;
		default:
			g_assert_not_reached ();
		}
		values [MONO_METHODIMPL_DECLARATION] = tok;
	}

	if (mb->generic_params) {
		table = &assembly->tables [MONO_TABLE_GENERICPARAM];
		table->rows += mono_array_length (mb->generic_params);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (mb->generic_params); ++i) {
			guint32 owner = MONO_TYPEORMETHOD_METHOD | (mb->table_idx << MONO_TYPEORMETHOD_BITS);

			mono_image_get_generic_param_info (
				mono_array_get (mb->generic_params, gpointer, i), owner, assembly);
		}
	}

}

static void
mono_image_get_ctor_info (MonoDomain *domain, MonoReflectionCtorBuilder *mb, MonoDynamicImage *assembly)
{
	ReflectionMethodBuilder rmb;

	reflection_methodbuilder_from_ctor_builder (&rmb, mb);

	mono_image_basic_method (&rmb, assembly);
}

static guint32
fieldref_encode_signature (MonoDynamicImage *assembly, MonoType *type)
{
	char blob_size [64];
	char *b = blob_size;
	char *p;
	char* buf;
	guint32 idx;

	if (!assembly->save)
		return 0;

	p = buf = g_malloc (64);
	
	mono_metadata_encode_value (0x06, p, &p);
	/* encode custom attributes before the type */
	encode_type (assembly, type, p, &p);
	g_assert (p-buf < 64);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);
	return idx;
}

static guint32
field_encode_signature (MonoDynamicImage *assembly, MonoReflectionFieldBuilder *fb)
{
	char blob_size [64];
	char *b = blob_size;
	char *p;
	char* buf;
	guint32 idx;
	
	p = buf = g_malloc (64);
	
	mono_metadata_encode_value (0x06, p, &p);
	/* encode custom attributes before the type */
	encode_reflection_type (assembly, fb->type, p, &p);
	g_assert (p-buf < 64);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);
	return idx;
}

/*
 * Copy len * nelem bytes from val to dest, swapping bytes to LE if necessary.
 * dest may be misaligned.
 */
static void
swap_with_size (char *dest, const char* val, int len, int nelem) {
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	int elem;

	for (elem = 0; elem < nelem; ++elem) {
		switch (len) {
		case 1:
			*dest = *val;
			break;
		case 2:
			dest [0] = val [1];
			dest [1] = val [0];
			break;
		case 4:
			dest [0] = val [3];
			dest [1] = val [2];
			dest [2] = val [1];
			dest [3] = val [0];
			break;
		case 8:
			dest [0] = val [7];
			dest [1] = val [6];
			dest [2] = val [5];
			dest [3] = val [4];
			dest [4] = val [3];
			dest [5] = val [2];
			dest [6] = val [1];
			dest [7] = val [0];
			break;
		default:
			g_assert_not_reached ();
		}
		dest += len;
		val += len;
	}
#else
	memcpy (dest, val, len * nelem);
#endif
}

static guint32
encode_constant (MonoDynamicImage *assembly, MonoObject *val, guint32 *ret_type) {
	char blob_size [64];
	char *b = blob_size;
	char *p, *box_val;
	char* buf;
	guint32 idx, len, dummy = 0;
	
	p = buf = g_malloc (64);
	if (!val) {
		*ret_type = MONO_TYPE_CLASS;
		len = 4;
		box_val = (char*)&dummy;
	} else {
		box_val = ((char*)val) + sizeof (MonoObject);
		*ret_type = val->vtable->klass->byval_arg.type;
	}
handle_enum:
	switch (*ret_type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
		len = 1;
		break;
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
		len = 2;
		break;
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_R4:
		len = 4;
		break;
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
	case MONO_TYPE_R8:
		len = 8;
		break;
	case MONO_TYPE_VALUETYPE:
		if (val->vtable->klass->enumtype) {
			*ret_type = val->vtable->klass->enum_basetype->type;
			goto handle_enum;
		} else
			g_error ("we can't encode valuetypes");
	case MONO_TYPE_CLASS:
		break;
	case MONO_TYPE_STRING: {
		MonoString *str = (MonoString*)val;
		/* there is no signature */
		len = str->length * 2;
		mono_metadata_encode_value (len, b, &b);
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
		{
			char *swapped = g_malloc (2 * mono_string_length (str));
			const char *p = (const char*)mono_string_chars (str);

			swap_with_size (swapped, p, 2, mono_string_length (str));
			idx = add_to_blob_cached (assembly, blob_size, b-blob_size, swapped, len);
			g_free (swapped);
		}
#else
		idx = add_to_blob_cached (assembly, blob_size, b-blob_size, (char*)mono_string_chars (str), len);
#endif

		g_free (buf);
		return idx;
	}
	default:
		g_error ("we don't encode constant type 0x%02x yet", *ret_type);
	}

	/* there is no signature */
	mono_metadata_encode_value (len, b, &b);
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	idx = mono_image_add_stream_data (&assembly->blob, blob_size, b-blob_size);
	swap_with_size (blob_size, box_val, len, 1);
	mono_image_add_stream_data (&assembly->blob, blob_size, len);
#else
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, box_val, len);
#endif

	g_free (buf);
	return idx;
}

static guint32
encode_marshal_blob (MonoDynamicImage *assembly, MonoReflectionMarshal *minfo) {
	char blob_size [64];
	char *b = blob_size;
	char *p, *buf, *str;
	guint32 idx, len, bufsize = 256;
	
	p = buf = g_malloc (bufsize);

	switch (minfo->type) {
	case MONO_NATIVE_BYVALTSTR:
	case MONO_NATIVE_BYVALARRAY:
		mono_metadata_encode_value (minfo->type, p, &p);
		mono_metadata_encode_value (minfo->count, p, &p);
		break;
		/* FIXME: handle ARRAY and other unmanaged types that need extra info */
	case MONO_NATIVE_CUSTOM:
		mono_metadata_encode_value (minfo->type, p, &p);
		if (minfo->guid) {
			str = mono_string_to_utf8 (minfo->guid);
			len = strlen (str);
			mono_metadata_encode_value (len, p, &p);
			memcpy (p, str, len);
			p += len;
			g_free (str);
		} else {
			mono_metadata_encode_value (0, p, &p);
		}
		if (minfo->marshaltype) {
			str = mono_string_to_utf8 (minfo->marshaltype);
			len = strlen (str);
			mono_metadata_encode_value (len, p, &p);
			if (p + len >= buf + bufsize) {
				idx = p - buf;
				bufsize *= 2;
				buf = g_realloc (buf, bufsize);
				p = buf + idx;
			}
			memcpy (p, str, len);
			p += len;
			g_free (str);
		} else {
			mono_metadata_encode_value (0, p, &p);
		}
		if (minfo->marshaltyperef) {
			str = type_get_qualified_name (minfo->marshaltyperef->type, assembly->image.assembly);
			len = strlen (str);
			mono_metadata_encode_value (len, p, &p);
			if (p + len >= buf + bufsize) {
				idx = p - buf;
				bufsize *= 2;
				buf = g_realloc (buf, bufsize);
				p = buf + idx;
			}
			memcpy (p, str, len);
			p += len;
			g_free (str);
		} else {
			mono_metadata_encode_value (0, p, &p);
		}
		if (minfo->mcookie) {
			str = mono_string_to_utf8 (minfo->mcookie);
			len = strlen (str);
			mono_metadata_encode_value (len, p, &p);
			if (p + len >= buf + bufsize) {
				idx = p - buf;
				bufsize *= 2;
				buf = g_realloc (buf, bufsize);
				p = buf + idx;
			}
			memcpy (p, str, len);
			p += len;
			g_free (str);
		} else {
			mono_metadata_encode_value (0, p, &p);
		}
		break;
	default:
		mono_metadata_encode_value (minfo->type, p, &p);
		break;
	}
	len = p-buf;
	mono_metadata_encode_value (len, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, len);
	g_free (buf);
	return idx;
}

static void
mono_image_get_field_info (MonoReflectionFieldBuilder *fb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 *values;
	char *name;

	/* maybe this fixup should be done in the C# code */
	if (fb->attrs & FIELD_ATTRIBUTE_LITERAL)
		fb->attrs |= FIELD_ATTRIBUTE_HAS_DEFAULT;
	table = &assembly->tables [MONO_TABLE_FIELD];
	fb->table_idx = table->next_idx ++;
	mono_g_hash_table_insert (assembly->field_to_table_idx, fb->handle, GUINT_TO_POINTER (fb->table_idx));
	values = table->values + fb->table_idx * MONO_FIELD_SIZE;
	name = mono_string_to_utf8 (fb->name);
	values [MONO_FIELD_NAME] = string_heap_insert (&assembly->sheap, name);
	g_free (name);
	values [MONO_FIELD_FLAGS] = fb->attrs;
	values [MONO_FIELD_SIGNATURE] = field_encode_signature (assembly, fb);

	if (fb->offset != -1) {
		table = &assembly->tables [MONO_TABLE_FIELDLAYOUT];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_FIELD_LAYOUT_SIZE;
		values [MONO_FIELD_LAYOUT_FIELD] = fb->table_idx;
		values [MONO_FIELD_LAYOUT_OFFSET] = fb->offset;
	}
	if (fb->attrs & FIELD_ATTRIBUTE_LITERAL) {
		guint32 field_type = 0;
		table = &assembly->tables [MONO_TABLE_CONSTANT];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_CONSTANT_SIZE;
		values [MONO_CONSTANT_PARENT] = HASCONSTANT_FIEDDEF | (fb->table_idx << HASCONSTANT_BITS);
		values [MONO_CONSTANT_VALUE] = encode_constant (assembly, fb->def_value, &field_type);
		values [MONO_CONSTANT_TYPE] = field_type;
		values [MONO_CONSTANT_PADDING] = 0;
	}
	if (fb->rva_data) {
		guint32 rva_idx;
		table = &assembly->tables [MONO_TABLE_FIELDRVA];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_FIELD_RVA_SIZE;
		values [MONO_FIELD_RVA_FIELD] = fb->table_idx;
		/*
		 * We store it in the code section because it's simpler for now.
		 */
		rva_idx = mono_image_add_stream_data (&assembly->code, mono_array_addr (fb->rva_data, char, 0), mono_array_length (fb->rva_data));
		values [MONO_FIELD_RVA_RVA] = rva_idx + assembly->text_rva;
	}
	if (fb->marshal_info) {
		table = &assembly->tables [MONO_TABLE_FIELDMARSHAL];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_FIELD_MARSHAL_SIZE;
		values [MONO_FIELD_MARSHAL_PARENT] = (fb->table_idx << HAS_FIELD_MARSHAL_BITS) | HAS_FIELD_MARSHAL_FIELDSREF;
		values [MONO_FIELD_MARSHAL_NATIVE_TYPE] = encode_marshal_blob (assembly, fb->marshal_info);
	}
}

static guint32
property_encode_signature (MonoDynamicImage *assembly, MonoReflectionPropertyBuilder *fb)
{
	char *buf, *p;
	char blob_size [6];
	char *b = blob_size;
	guint32 nparams = 0;
	MonoReflectionMethodBuilder *mb = fb->get_method;
	MonoReflectionMethodBuilder *smb = fb->set_method;
	guint32 idx, i, size;

	if (mb && mb->parameters)
		nparams = mono_array_length (mb->parameters);
	if (!mb && smb && smb->parameters)
		nparams = mono_array_length (smb->parameters) - 1;
	size = 24 + nparams * 10;
	buf = p = g_malloc (size);
	*p = 0x08;
	p++;
	mono_metadata_encode_value (nparams, p, &p);
	if (mb) {
		encode_reflection_type (assembly, mb->rtype, p, &p);
		for (i = 0; i < nparams; ++i) {
			MonoReflectionType *pt = mono_array_get (mb->parameters, MonoReflectionType*, i);
			encode_reflection_type (assembly, pt, p, &p);
		}
	} else {
		/* the property type is the last param */
		encode_reflection_type (assembly, mono_array_get (smb->parameters, MonoReflectionType*, nparams), p, &p);
		for (i = 0; i < nparams; ++i) {
			MonoReflectionType *pt = mono_array_get (smb->parameters, MonoReflectionType*, i);
			encode_reflection_type (assembly, pt, p, &p);
		}
	}
	/* store length */
	g_assert (p - buf < size);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);
	return idx;
}

static void
mono_image_get_property_info (MonoReflectionPropertyBuilder *pb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 *values;
	char *name;
	guint num_methods = 0;
	guint32 semaidx;

	/* 
	 * we need to set things in the following tables:
	 * PROPERTYMAP (info already filled in _get_type_info ())
	 * PROPERTY    (rows already preallocated in _get_type_info ())
	 * METHOD      (method info already done with the generic method code)
	 * METHODSEMANTICS
	 */
	table = &assembly->tables [MONO_TABLE_PROPERTY];
	pb->table_idx = table->next_idx ++;
	values = table->values + pb->table_idx * MONO_PROPERTY_SIZE;
	name = mono_string_to_utf8 (pb->name);
	values [MONO_PROPERTY_NAME] = string_heap_insert (&assembly->sheap, name);
	g_free (name);
	values [MONO_PROPERTY_FLAGS] = pb->attrs;
	values [MONO_PROPERTY_TYPE] = property_encode_signature (assembly, pb);

	/* FIXME: we still don't handle 'other' methods */
	if (pb->get_method) num_methods ++;
	if (pb->set_method) num_methods ++;

	table = &assembly->tables [MONO_TABLE_METHODSEMANTICS];
	table->rows += num_methods;
	alloc_table (table, table->rows);

	if (pb->get_method) {
		semaidx = table->next_idx ++;
		values = table->values + semaidx * MONO_METHOD_SEMA_SIZE;
		values [MONO_METHOD_SEMA_SEMANTICS] = METHOD_SEMANTIC_GETTER;
		values [MONO_METHOD_SEMA_METHOD] = pb->get_method->table_idx;
		values [MONO_METHOD_SEMA_ASSOCIATION] = (pb->table_idx << HAS_SEMANTICS_BITS) | HAS_SEMANTICS_PROPERTY;
	}
	if (pb->set_method) {
		semaidx = table->next_idx ++;
		values = table->values + semaidx * MONO_METHOD_SEMA_SIZE;
		values [MONO_METHOD_SEMA_SEMANTICS] = METHOD_SEMANTIC_SETTER;
		values [MONO_METHOD_SEMA_METHOD] = pb->set_method->table_idx;
		values [MONO_METHOD_SEMA_ASSOCIATION] = (pb->table_idx << HAS_SEMANTICS_BITS) | HAS_SEMANTICS_PROPERTY;
	}
}

static void
mono_image_get_event_info (MonoReflectionEventBuilder *eb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 *values;
	char *name;
	guint num_methods = 0;
	guint32 semaidx;

	/* 
	 * we need to set things in the following tables:
	 * EVENTMAP (info already filled in _get_type_info ())
	 * EVENT    (rows already preallocated in _get_type_info ())
	 * METHOD      (method info already done with the generic method code)
	 * METHODSEMANTICS
	 */
	table = &assembly->tables [MONO_TABLE_EVENT];
	eb->table_idx = table->next_idx ++;
	values = table->values + eb->table_idx * MONO_EVENT_SIZE;
	name = mono_string_to_utf8 (eb->name);
	values [MONO_EVENT_NAME] = string_heap_insert (&assembly->sheap, name);
	g_free (name);
	values [MONO_EVENT_FLAGS] = eb->attrs;
	values [MONO_EVENT_TYPE] = mono_image_typedef_or_ref (assembly, eb->type->type);

	/*
	 * FIXME: we still don't handle 'other' methods 
	 */
	if (eb->add_method) num_methods ++;
	if (eb->remove_method) num_methods ++;
	if (eb->raise_method) num_methods ++;

	table = &assembly->tables [MONO_TABLE_METHODSEMANTICS];
	table->rows += num_methods;
	alloc_table (table, table->rows);

	if (eb->add_method) {
		semaidx = table->next_idx ++;
		values = table->values + semaidx * MONO_METHOD_SEMA_SIZE;
		values [MONO_METHOD_SEMA_SEMANTICS] = METHOD_SEMANTIC_ADD_ON;
		values [MONO_METHOD_SEMA_METHOD] = eb->add_method->table_idx;
		values [MONO_METHOD_SEMA_ASSOCIATION] = (eb->table_idx << HAS_SEMANTICS_BITS) | HAS_SEMANTICS_EVENT;
	}
	if (eb->remove_method) {
		semaidx = table->next_idx ++;
		values = table->values + semaidx * MONO_METHOD_SEMA_SIZE;
		values [MONO_METHOD_SEMA_SEMANTICS] = METHOD_SEMANTIC_REMOVE_ON;
		values [MONO_METHOD_SEMA_METHOD] = eb->remove_method->table_idx;
		values [MONO_METHOD_SEMA_ASSOCIATION] = (eb->table_idx << HAS_SEMANTICS_BITS) | HAS_SEMANTICS_EVENT;
	}
	if (eb->raise_method) {
		semaidx = table->next_idx ++;
		values = table->values + semaidx * MONO_METHOD_SEMA_SIZE;
		values [MONO_METHOD_SEMA_SEMANTICS] = METHOD_SEMANTIC_FIRE;
		values [MONO_METHOD_SEMA_METHOD] = eb->raise_method->table_idx;
		values [MONO_METHOD_SEMA_ASSOCIATION] = (eb->table_idx << HAS_SEMANTICS_BITS) | HAS_SEMANTICS_EVENT;
	}
}

static void
encode_constraints (MonoReflectionGenericParam *gparam, guint32 owner, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 num_constraints, i;
	guint32 *values;
	guint32 table_idx;

	table = &assembly->tables [MONO_TABLE_GENERICPARAMCONSTRAINT];
	num_constraints = gparam ? mono_array_length (gparam->constraints) : 0;
	table->rows += num_constraints;
	alloc_table (table, table->rows);

	for (i = 0; i < num_constraints; i++) {
		MonoReflectionType *constraint = mono_array_get (gparam->constraints, gpointer, i);

		table_idx = table->next_idx ++;
		values = table->values + table_idx * MONO_GENPARCONSTRAINT_SIZE;

		values [MONO_GENPARCONSTRAINT_GENERICPAR] = owner;
		values [MONO_GENPARCONSTRAINT_CONSTRAINT] = mono_image_typedef_or_ref (assembly, constraint->type);
	}
}

static void
mono_image_get_generic_param_info (MonoReflectionGenericParam *gparam, guint32 owner, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 table_idx;

	table = &assembly->tables [MONO_TABLE_GENERICPARAM];
	table_idx = table->next_idx ++;
	values = table->values + table_idx * MONO_GENERICPARAM_SIZE;

	values [MONO_GENERICPARAM_OWNER] = owner;
	values [MONO_GENERICPARAM_FLAGS] = gparam->param->flags;
	values [MONO_GENERICPARAM_NUMBER] = gparam->param->num;
	values [MONO_GENERICPARAM_NAME] = string_heap_insert (&assembly->sheap, gparam->param->name);
	values [MONO_GENERICPARAM_KIND] = 0;
	values [MONO_GENERICPARAM_DEPRECATED_CONSTRAINT] = 0;

	if (gparam->constraints)
		encode_constraints (gparam, table_idx, assembly);
}

static guint32
resolution_scope_from_image (MonoDynamicImage *assembly, MonoImage *image)
{
	MonoDynamicTable *table;
	guint32 token;
	guint32 *values;
	guint32 cols [MONO_ASSEMBLY_SIZE];
	const char *pubkey;
	guint32 publen;

	if ((token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, image))))
		return token;

	if (image->assembly->dynamic && (image->assembly == assembly->image.assembly)) {
		table = &assembly->tables [MONO_TABLE_FILE];
		table->rows++;
		alloc_table (table, table->rows);
		values = table->values + table->next_idx * MONO_FILE_SIZE;
		values [MONO_FILE_FLAGS] = FILE_CONTAINS_METADATA;
		values [MONO_FILE_NAME] = string_heap_insert (&assembly->sheap, image->module_name);

		table = &assembly->tables [MONO_TABLE_MODULEREF];
		token = table->next_idx ++;
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + token * MONO_MODULEREF_SIZE;
		values [MONO_MODULEREF_NAME] = string_heap_insert (&assembly->sheap, image->module_name);

		token <<= RESOLTION_SCOPE_BITS;
		token |= RESOLTION_SCOPE_MODULEREF;
		g_hash_table_insert (assembly->handleref, image, GUINT_TO_POINTER (token));

		return token;
	}
	
	if (image->assembly->dynamic)
		/* FIXME: */
		memset (cols, 0, sizeof (cols));
	else
		mono_metadata_decode_row (&image->tables [MONO_TABLE_ASSEMBLY], 0, cols, MONO_ASSEMBLY_SIZE);

	table = &assembly->tables [MONO_TABLE_ASSEMBLYREF];
	token = table->next_idx ++;
	table->rows ++;
	alloc_table (table, table->rows);
	values = table->values + token * MONO_ASSEMBLYREF_SIZE;
	if (strcmp ("corlib", image->assembly_name) == 0)
		values [MONO_ASSEMBLYREF_NAME] = string_heap_insert (&assembly->sheap, "mscorlib");
	else
		values [MONO_ASSEMBLYREF_NAME] = string_heap_insert (&assembly->sheap, image->assembly_name);
	values [MONO_ASSEMBLYREF_MAJOR_VERSION] = cols [MONO_ASSEMBLY_MAJOR_VERSION];
	values [MONO_ASSEMBLYREF_MINOR_VERSION] = cols [MONO_ASSEMBLY_MINOR_VERSION];
	values [MONO_ASSEMBLYREF_BUILD_NUMBER] = cols [MONO_ASSEMBLY_BUILD_NUMBER];
	values [MONO_ASSEMBLYREF_REV_NUMBER] = cols [MONO_ASSEMBLY_REV_NUMBER];
	values [MONO_ASSEMBLYREF_FLAGS] = 0;
	values [MONO_ASSEMBLYREF_CULTURE] = 0;
	values [MONO_ASSEMBLYREF_HASH_VALUE] = 0;

	if ((pubkey = mono_image_get_public_key (image, &publen))) {
		guchar pubtoken [9];
		pubtoken [0] = 8;
		mono_digest_get_public_token (pubtoken + 1, pubkey, publen);
		values [MONO_ASSEMBLYREF_PUBLIC_KEY] = mono_image_add_stream_data (&assembly->blob, pubtoken, 9);
	} else {
		/* 
		 * We add the pubtoken from ms, so that the ms runtime can handle our binaries.
		 * This is currently only a problem with references to System.Xml (see bug#27706),
		 * but there may be other cases that makes this necessary. Note, we need to set 
		 * the version as well. When/if we sign our assemblies, we'd need to get our pubtoken 
		 * recognized by ms, yuck!
		 * FIXME: need to add more assembly names, as needed.
		 */
		if (strcmp (image->assembly_name, "corlib") == 0 ||
				strcmp (image->assembly_name, "mscorlib") == 0 ||
				strcmp (image->assembly_name, "System") == 0 ||
				strcmp (image->assembly_name, "System.Runtime.Remoting") == 0 ||
				strcmp (image->assembly_name, "System.Xml") == 0 ||
				strcmp (image->assembly_name, "System.Data") == 0 ||
				strcmp (image->assembly_name, "System.Windows.Forms") == 0) {
			static const guchar ptoken [9] = {8, '\xB7', '\x7A', '\x5C', '\x56', '\x19', '\x34', '\xE0', '\x89'};
			values [MONO_ASSEMBLYREF_PUBLIC_KEY] = mono_image_add_stream_data (&assembly->blob, ptoken, 9);
			values [MONO_ASSEMBLYREF_MAJOR_VERSION] = 1;
			values [MONO_ASSEMBLYREF_BUILD_NUMBER] = 3300;
		} else if (strcmp (image->assembly_name, "Accessibility") == 0 ||
				strcmp (image->assembly_name, "cscompmgd") == 0 ||
				strcmp (image->assembly_name, "CustomMarshalers") == 0 ||
				strcmp (image->assembly_name, "Microsoft.JScript") == 0 ||
				strcmp (image->assembly_name, "Microsoft.VisualBasic") == 0 ||
				strcmp (image->assembly_name, "Microsoft.VisualBasic.Vsa") == 0 ||
				strcmp (image->assembly_name, "Microsoft.VisualC") == 0 ||
				strcmp (image->assembly_name, "Microsoft.Vsa") == 0 ||
				strcmp (image->assembly_name, "System.Configuration.Install") == 0 ||
				strcmp (image->assembly_name, "System.DirectoryServices") == 0 ||
				strcmp (image->assembly_name, "System.Design") == 0 ||
				strcmp (image->assembly_name, "System.Drawing") == 0 ||
				strcmp (image->assembly_name, "System.Drawing.Design") == 0 ||
				strcmp (image->assembly_name, "System.EnterpriseServices") == 0 ||
				strcmp (image->assembly_name, "System.Management") == 0 ||
				strcmp (image->assembly_name, "System.Messaging") == 0 ||
				strcmp (image->assembly_name, "System.Runtime.Serialization.Formatters.Soap") == 0 ||
				strcmp (image->assembly_name, "System.Security") == 0 ||
				strcmp (image->assembly_name, "System.ServiceProcess") == 0 ||
				strcmp (image->assembly_name, "System.Web.Services") == 0 ||
				strcmp (image->assembly_name, "CustomMarshalers") == 0 ||
				strcmp (image->assembly_name, "System.Web") == 0) {
			static const guchar ptoken [9] = {8, '\xb0', '\x3f', '\x5f', '\x7f', '\x11', '\xd5', '\x0a', '\x3a'};
			values [MONO_ASSEMBLYREF_PUBLIC_KEY] = mono_image_add_stream_data (&assembly->blob, ptoken, 9);
			values [MONO_ASSEMBLYREF_MAJOR_VERSION] = 1;
			values [MONO_ASSEMBLYREF_BUILD_NUMBER] = 3300;
		} else {
			values [MONO_ASSEMBLYREF_PUBLIC_KEY] = 0;
		}
	}
	token <<= RESOLTION_SCOPE_BITS;
	token |= RESOLTION_SCOPE_ASSEMBLYREF;
	g_hash_table_insert (assembly->handleref, image, GUINT_TO_POINTER (token));
	return token;
}

static guint32
create_typespec (MonoDynamicImage *assembly, MonoType *type)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 token;
	char sig [128];
	char *p = sig;
	char blob_size [6];
	char *b = blob_size;

	switch (type->type) {
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_PTR:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
	case MONO_TYPE_GENERICINST:
		encode_type (assembly, type, p, &p);
		break;
	case MONO_TYPE_CLASS:
	case MONO_TYPE_VALUETYPE: {
		MonoClass *k = mono_class_from_mono_type (type);
		if (!k || !k->generic_inst)
			return 0;
		encode_type (assembly, k->generic_inst, p, &p);
		break;
	}
	default:
		return 0;
	}

	table = &assembly->tables [MONO_TABLE_TYPESPEC];
	if (assembly->save) {
		g_assert (p-sig < 128);
		mono_metadata_encode_value (p-sig, b, &b);
		token = add_to_blob_cached (assembly, blob_size, b-blob_size, sig, p-sig);
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_TYPESPEC_SIZE;
		values [MONO_TYPESPEC_SIGNATURE] = token;
	}

	token = TYPEDEFORREF_TYPESPEC | (table->next_idx << TYPEDEFORREF_BITS);
	g_hash_table_insert (assembly->typeref, type, GUINT_TO_POINTER(token));
	table->next_idx ++;
	return token;
}

/*
 * Despite the name, we handle also TypeSpec (with the above helper).
 */
static guint32
mono_image_typedef_or_ref (MonoDynamicImage *assembly, MonoType *type)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 token, scope, enclosing;
	MonoClass *klass;

	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->typeref, type));
	if (token)
		return token;
	token = create_typespec (assembly, type);
	if (token)
		return token;
	klass = my_mono_class_from_mono_type (type);
	if (!klass)
		klass = mono_class_from_mono_type (type);

	/*
	 * If it's in the same module and not a generic type parameter:
	 */
	if ((klass->image == &assembly->image) &&
	    (type->type != MONO_TYPE_VAR) && (type->type != MONO_TYPE_MVAR)) {
		MonoReflectionTypeBuilder *tb = klass->reflection_info;
		token = TYPEDEFORREF_TYPEDEF | (tb->table_idx << TYPEDEFORREF_BITS);
		mono_g_hash_table_insert (assembly->tokens, GUINT_TO_POINTER (token), klass->reflection_info);
		return token;
	}

	if (klass->nested_in) {
		enclosing = mono_image_typedef_or_ref (assembly, &klass->nested_in->byval_arg);
		/* get the typeref idx of the enclosing type */
		enclosing >>= TYPEDEFORREF_BITS;
		scope = (enclosing << RESOLTION_SCOPE_BITS) | RESOLTION_SCOPE_TYPEREF;
	} else {
		scope = resolution_scope_from_image (assembly, klass->image);
	}
	table = &assembly->tables [MONO_TABLE_TYPEREF];
	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_TYPEREF_SIZE;
		values [MONO_TYPEREF_SCOPE] = scope;
		values [MONO_TYPEREF_NAME] = string_heap_insert (&assembly->sheap, klass->name);
		values [MONO_TYPEREF_NAMESPACE] = string_heap_insert (&assembly->sheap, klass->name_space);
	}
	token = TYPEDEFORREF_TYPEREF | (table->next_idx << TYPEDEFORREF_BITS); /* typeref */
	g_hash_table_insert (assembly->typeref, type, GUINT_TO_POINTER(token));
	table->next_idx ++;
	mono_g_hash_table_insert (assembly->tokens, GUINT_TO_POINTER (token), klass->reflection_info);
	return token;
}

/*
 * Insert a memberef row into the metadata: the token that point to the memberref
 * is returned. Caching is done in the caller (mono_image_get_methodref_token() or
 * mono_image_get_fieldref_token()).
 * The sig param is an index to an already built signature.
 */
static guint32
mono_image_get_memberref_token (MonoDynamicImage *assembly, MonoType *type, const char *name, guint32 sig)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 token, pclass;
	guint32 parent;

	parent = mono_image_typedef_or_ref (assembly, type);
	switch (parent & TYPEDEFORREF_MASK) {
	case TYPEDEFORREF_TYPEREF:
		pclass = MEMBERREF_PARENT_TYPEREF;
		break;
	case TYPEDEFORREF_TYPESPEC:
		pclass = MEMBERREF_PARENT_TYPESPEC;
		break;
	case TYPEDEFORREF_TYPEDEF:
		pclass = MEMBERREF_PARENT_TYPEDEF;
		break;
	default:
		g_warning ("unknown typeref or def token 0x%08x for %s", parent, name);
		return 0;
	}
	/* extract the index */
	parent >>= TYPEDEFORREF_BITS;

	table = &assembly->tables [MONO_TABLE_MEMBERREF];

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_MEMBERREF_SIZE;
		values [MONO_MEMBERREF_CLASS] = pclass | (parent << MEMBERREF_PARENT_BITS);
		values [MONO_MEMBERREF_NAME] = string_heap_insert (&assembly->sheap, name);
		values [MONO_MEMBERREF_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_MEMBER_REF | table->next_idx;
	table->next_idx ++;

	return token;
}

static guint32
mono_image_get_methodref_token (MonoDynamicImage *assembly, MonoMethod *method)
{
	guint32 token;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, method));
	if (token)
		return token;
	token = mono_image_get_memberref_token (assembly, &method->klass->byval_arg,
		method->name,  method_encode_signature (assembly, method->signature));
	g_hash_table_insert (assembly->handleref, method, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_methodbuilder_token (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *mb)
{
	guint32 token;
	ReflectionMethodBuilder rmb;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, mb));
	if (token)
		return token;

	reflection_methodbuilder_from_method_builder (&rmb, mb);

	
	token = mono_image_get_memberref_token (assembly, ((MonoReflectionTypeBuilder*)rmb.type)->type.type,
											mono_string_to_utf8 (rmb.name),
											method_builder_encode_signature (assembly, &rmb));
	g_hash_table_insert (assembly->handleref, mb, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_ctorbuilder_token (MonoDynamicImage *assembly, MonoReflectionCtorBuilder *mb)
{
	guint32 token;
	ReflectionMethodBuilder rmb;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, mb));
	if (token)
		return token;

	reflection_methodbuilder_from_ctor_builder (&rmb, mb);

	token = mono_image_get_memberref_token (assembly, ((MonoReflectionTypeBuilder*)rmb.type)->type.type,
											mono_string_to_utf8 (rmb.name),
											method_builder_encode_signature (assembly, &rmb));
	g_hash_table_insert (assembly->handleref, mb, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_fieldref_token (MonoDynamicImage *assembly, MonoClassField *field, MonoClass *klass)
{
	guint32 token;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, field));
	if (token)
		return token;
	field->parent = klass;
	token = mono_image_get_memberref_token (assembly, &klass->byval_arg, 
		field->name,  fieldref_encode_signature (assembly, field->type));
	g_hash_table_insert (assembly->handleref, field, GUINT_TO_POINTER(token));
	return token;
}

static guint32
field_encode_inflated_field (MonoDynamicImage *assembly, MonoReflectionInflatedField *field)
{
	guint32 sig, token;
	MonoClass *klass;
	const gchar *name;

	klass = field->rfield.klass;
	name = field->rfield.field->name;
	sig = fieldref_encode_signature (assembly, field->declaring->type);
	token = mono_image_get_memberref_token (assembly, &klass->byval_arg, name, sig);

	return token;
}

static guint32
mono_image_get_inflated_field_token (MonoDynamicImage *assembly, MonoReflectionInflatedField *field)
{
	guint32 token;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, field->rfield.field));
	if (token)
		return token;
	token = field_encode_inflated_field (assembly, field);
	g_hash_table_insert (assembly->handleref, field, GUINT_TO_POINTER(token));
	return token;
}

static guint32
encode_generic_method_sig (MonoDynamicImage *assembly, MonoGenericInst *ginst)
{
	char *buf;
	char *p;
	int i;
	guint32 nparams =  ginst->type_argc;
	guint32 size = 10 + nparams * 10;
	guint32 idx;
	char blob_size [6];
	char *b = blob_size;

	if (!assembly->save)
		return 0;

	p = buf = g_malloc (size);
	/*
	 * FIXME: vararg, explicit_this, differenc call_conv values...
	 */
	mono_metadata_encode_value (0xa, p, &p); /// FIXME FIXME FIXME
	mono_metadata_encode_value (nparams, p, &p);

	for (i = 0; i < nparams; i++)
		encode_type (assembly, ginst->type_argv [i], p, &p);

	/* store length */
	g_assert (p - buf < size);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);
	return idx;
}

static guint32
method_encode_methodspec (MonoDynamicImage *assembly, MonoGenericInst *ginst)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 token, mtoken, sig;

	table = &assembly->tables [MONO_TABLE_METHODSPEC];

	g_assert (ginst);
	if (ginst->generic_method) {
		MonoMethod *gm = ginst->generic_method;
		MonoClass *k = ginst->klass ? ginst->klass : gm->klass;

		sig = method_encode_signature (assembly, gm->signature);
		mtoken = mono_image_get_memberref_token (assembly, &k->byval_arg, gm->name, sig);

		if (!ginst->generic_method->signature->generic_param_count)
			return mtoken;
	}
	else
		g_assert_not_reached ();

	switch (mono_metadata_token_table (mtoken)) {
	case MONO_TABLE_MEMBERREF:
		mtoken = (mono_metadata_token_index (mtoken) << METHODDEFORREF_BITS) | METHODDEFORREF_METHODREF;
		break;
	case MONO_TABLE_METHOD:
		mtoken = (mono_metadata_token_index (mtoken) << METHODDEFORREF_BITS) | METHODDEFORREF_METHODDEF;
		break;
	default:
		g_assert_not_reached ();
	}

	sig = encode_generic_method_sig (assembly, ginst);

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_METHODSPEC_SIZE;
		values [MONO_METHODSPEC_METHOD] = mtoken;
		values [MONO_METHODSPEC_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_METHOD_SPEC | table->next_idx;
	table->next_idx ++;

	return token;
}

static guint32
mono_image_get_methodspec_token (MonoDynamicImage *assembly, MonoMethod *method, MonoGenericInst *ginst)
{
	guint32 token;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, method));
	if (token)
		return token;
	token = method_encode_methodspec (assembly, ginst);
	g_hash_table_insert (assembly->handleref, method, GUINT_TO_POINTER(token));
	return token;
}

static guint32
create_generic_typespec (MonoDynamicImage *assembly, MonoReflectionTypeBuilder *tb)
{
	MonoDynamicTable *table;
	MonoClass *klass;
	guint32 *values;
	guint32 token;
	char sig [128];
	char *p = sig;
	char blob_size [6];
	char *b = blob_size;
	int count, i;

	g_assert (tb->generic_params);
	klass = mono_class_from_mono_type (tb->type.type);

	mono_metadata_encode_value (MONO_TYPE_GENERICINST, p, &p);
	encode_type (assembly, &klass->byval_arg, p, &p);

	count = mono_array_length (tb->generic_params);
	mono_metadata_encode_value (count, p, &p);
	for (i = 0; i < count; i++) {
		MonoReflectionGenericParam *gparam;
		MonoType t;

		gparam = mono_array_get (tb->generic_params, MonoReflectionGenericParam *, i);

		t.type = MONO_TYPE_VAR;
		t.data.generic_param = gparam->param;

		encode_type (assembly, &t, p, &p);
	}

	table = &assembly->tables [MONO_TABLE_TYPESPEC];
	if (assembly->save) {
		g_assert (p-sig < 128);
		mono_metadata_encode_value (p-sig, b, &b);
		token = add_to_blob_cached (assembly, blob_size, b-blob_size, sig, p-sig);
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_TYPESPEC_SIZE;
		values [MONO_TYPESPEC_SIGNATURE] = token;
	}

	token = TYPEDEFORREF_TYPESPEC | (table->next_idx << TYPEDEFORREF_BITS);
	g_hash_table_insert (assembly->typeref, tb->type.type, GUINT_TO_POINTER(token));
	table->next_idx ++;
	return token;
}

static guint32
mono_image_get_generic_field_token (MonoDynamicImage *assembly, MonoReflectionFieldBuilder *fb)
{
	MonoDynamicTable *table;
	MonoClass *klass;
	guint32 *values;
	guint32 token, pclass, parent, sig;
	gchar *name;

	klass = mono_class_from_mono_type (fb->typeb->type);
	name = mono_string_to_utf8 (fb->name);

	sig = fieldref_encode_signature (assembly, fb->type->type);

	parent = create_generic_typespec (assembly, (MonoReflectionTypeBuilder *) fb->typeb);
	g_assert ((parent & TYPEDEFORREF_MASK) == TYPEDEFORREF_TYPESPEC);
	
	pclass = MEMBERREF_PARENT_TYPESPEC;
	parent >>= TYPEDEFORREF_BITS;

	table = &assembly->tables [MONO_TABLE_MEMBERREF];

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_MEMBERREF_SIZE;
		values [MONO_MEMBERREF_CLASS] = pclass | (parent << MEMBERREF_PARENT_BITS);
		values [MONO_MEMBERREF_NAME] = string_heap_insert (&assembly->sheap, name);
		values [MONO_MEMBERREF_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_MEMBER_REF | table->next_idx;
	table->next_idx ++;

	return token;
}

static guint32
mono_reflection_encode_sighelper (MonoDynamicImage *assembly, MonoReflectionSigHelper *helper)
{
	char *buf;
	char *p;
	guint32 nargs;
	guint32 size;
	guint32 i, idx;
	char blob_size [6];
	char *b = blob_size;

	if (!assembly->save)
		return 0;

	/* FIXME: */
	g_assert (helper->type == 2);

	if (helper->arguments)
		nargs = mono_array_length (helper->arguments);
	else
		nargs = 0;

	size = 10 + (nargs * 10);
	
	p = buf = g_malloc (size);

	/* Encode calling convention */
	/* Change Any to Standard */
	if ((helper->call_conv & 0x03) == 0x03)
		helper->call_conv = 0x01;
	/* explicit_this implies has_this */
	if (helper->call_conv & 0x40)
		helper->call_conv &= 0x20;

	if (helper->call_conv == 0) /* Unmanaged */
		*p = helper->unmanaged_call_conv - 1;
	else {
		/* Managed */
		*p = helper->call_conv & 0x60; /* has_this + explicit_this */
		if (helper->call_conv & 0x02) /* varargs */
			*p += 0x05;
	}

	p++;
	mono_metadata_encode_value (nargs, p, &p);
	encode_reflection_type (assembly, helper->return_type, p, &p);
	for (i = 0; i < nargs; ++i) {
		MonoReflectionType *pt = mono_array_get (helper->arguments, MonoReflectionType*, i);
		encode_reflection_type (assembly, pt, p, &p);
	}
	/* store length */
	g_assert (p - buf < size);
	mono_metadata_encode_value (p-buf, b, &b);
	idx = add_to_blob_cached (assembly, blob_size, b-blob_size, buf, p-buf);
	g_free (buf);

	return idx;
}
	
static guint32 
mono_image_get_sighelper_token (MonoDynamicImage *assembly, MonoReflectionSigHelper *helper)
{
	guint32 idx;
	MonoDynamicTable *table;
	guint32 *values;

	table = &assembly->tables [MONO_TABLE_STANDALONESIG];
	idx = table->next_idx ++;
	table->rows ++;
	alloc_table (table, table->rows);
	values = table->values + idx * MONO_STAND_ALONE_SIGNATURE_SIZE;

	values [MONO_STAND_ALONE_SIGNATURE] =
		mono_reflection_encode_sighelper (assembly, helper);

	return idx;
}

static int
reflection_cc_to_file (int call_conv) {
	switch (call_conv & 0x3) {
	case 0:
	case 1: return MONO_CALL_DEFAULT;
	case 2: return MONO_CALL_VARARG;
	default:
		g_assert_not_reached ();
	}
	return 0;
}

typedef struct {
	MonoType *parent;
	MonoMethodSignature *sig;
	char *name;
	guint32 token;
} ArrayMethod;

static guint32
mono_image_get_array_token (MonoDynamicImage *assembly, MonoReflectionArrayMethod *m)
{
	guint32 nparams, i;
	GList *tmp;
	char *name;
	MonoMethodSignature *sig;
	ArrayMethod *am;
	
	name = mono_string_to_utf8 (m->name);
	nparams = mono_array_length (m->parameters);
	sig = g_malloc0 (sizeof (MonoMethodSignature) + sizeof (MonoType*) * nparams);
	sig->hasthis = 1;
	sig->call_convention = reflection_cc_to_file (m->call_conv);
	sig->param_count = nparams;
	sig->ret = m->ret? m->ret->type: &mono_defaults.void_class->byval_arg;
	for (i = 0; i < nparams; ++i) {
		MonoReflectionType *t = mono_array_get (m->parameters, gpointer, i);
		sig->params [i] = t->type;
	}

	for (tmp = assembly->array_methods; tmp; tmp = tmp->next) {
		am = tmp->data;
		if (strcmp (name, am->name) == 0 && 
				mono_metadata_type_equal (am->parent, m->parent->type) &&
				mono_metadata_signature_equal (am->sig, sig)) {
			g_free (name);
			g_free (sig);
			m->table_idx = am->token & 0xffffff;
			return am->token;
		}
	}
	am = g_new0 (ArrayMethod, 1);
	am->name = name;
	am->sig = sig;
	am->parent = m->parent->type;
	am->token = mono_image_get_memberref_token (assembly, am->parent,
			name,  method_encode_signature (assembly, sig));
	assembly->array_methods = g_list_prepend (assembly->array_methods, am);
	m->table_idx = am->token & 0xffffff;
	return am->token;
}

/*
 * Insert into the metadata tables all the info about the TypeBuilder tb.
 * Data in the tables is inserted in a predefined order, since some tables need to be sorted.
 */
static void
mono_image_get_type_info (MonoDomain *domain, MonoReflectionTypeBuilder *tb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	guint *values;
	int i, is_object = 0, is_system = 0;
	char *n;

	table = &assembly->tables [MONO_TABLE_TYPEDEF];
	values = table->values + tb->table_idx * MONO_TYPEDEF_SIZE;
	values [MONO_TYPEDEF_FLAGS] = tb->attrs;
	n = mono_string_to_utf8 (tb->name);
	if (strcmp (n, "Object") == 0)
		is_object++;
	values [MONO_TYPEDEF_NAME] = string_heap_insert (&assembly->sheap, n);
	g_free (n);
	n = mono_string_to_utf8 (tb->nspace);
	if (strcmp (n, "System") == 0)
		is_system++;
	values [MONO_TYPEDEF_NAMESPACE] = string_heap_insert (&assembly->sheap, n);
	g_free (n);
	if (tb->parent && !(is_system && is_object) && 
		!(tb->attrs & TYPE_ATTRIBUTE_INTERFACE)) { /* interfaces don't have a parent */
		values [MONO_TYPEDEF_EXTENDS] = mono_image_typedef_or_ref (assembly, tb->parent->type);
	} else
		values [MONO_TYPEDEF_EXTENDS] = 0;
	values [MONO_TYPEDEF_FIELD_LIST] = assembly->tables [MONO_TABLE_FIELD].next_idx;
	values [MONO_TYPEDEF_METHOD_LIST] = assembly->tables [MONO_TABLE_METHOD].next_idx;

	/*
	 * if we have explicitlayout or sequentiallayouts, output data in the
	 * ClassLayout table.
	 */
	if (((tb->attrs & TYPE_ATTRIBUTE_LAYOUT_MASK) != TYPE_ATTRIBUTE_AUTO_LAYOUT) && (tb->class_size != -1)) {
		table = &assembly->tables [MONO_TABLE_CLASSLAYOUT];
		table->rows++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_CLASS_LAYOUT_SIZE;
		values [MONO_CLASS_LAYOUT_PARENT] = tb->table_idx;
		values [MONO_CLASS_LAYOUT_CLASS_SIZE] = tb->class_size;
		values [MONO_CLASS_LAYOUT_PACKING_SIZE] = tb->packing_size;
	}

	/* handle interfaces */
	if (tb->interfaces) {
		table = &assembly->tables [MONO_TABLE_INTERFACEIMPL];
		i = table->rows;
		table->rows += mono_array_length (tb->interfaces);
		alloc_table (table, table->rows);
		values = table->values + (i + 1) * MONO_INTERFACEIMPL_SIZE;
		for (i = 0; i < mono_array_length (tb->interfaces); ++i) {
			MonoReflectionType* iface = (MonoReflectionType*) mono_array_get (tb->interfaces, gpointer, i);
			values [MONO_INTERFACEIMPL_CLASS] = tb->table_idx;
			values [MONO_INTERFACEIMPL_INTERFACE] = mono_image_typedef_or_ref (assembly, iface->type);
			values += MONO_INTERFACEIMPL_SIZE;
		}
	}

	/* handle generic parameters */
	if (tb->generic_params) {
		table = &assembly->tables [MONO_TABLE_GENERICPARAM];
		table->rows += mono_array_length (tb->generic_params);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (tb->generic_params); ++i) {
			guint32 owner = MONO_TYPEORMETHOD_TYPE | (tb->table_idx << MONO_TYPEORMETHOD_BITS);

			mono_image_get_generic_param_info (
				mono_array_get (tb->generic_params, MonoReflectionGenericParam*, i), owner, assembly);
		}
	}

	/* handle fields */
	if (tb->fields) {
		table = &assembly->tables [MONO_TABLE_FIELD];
		table->rows += mono_array_length (tb->fields);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (tb->fields); ++i)
			mono_image_get_field_info (
				mono_array_get (tb->fields, MonoReflectionFieldBuilder*, i), assembly);
	}

	/* handle constructors */
	if (tb->ctors) {
		table = &assembly->tables [MONO_TABLE_METHOD];
		table->rows += mono_array_length (tb->ctors);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (tb->ctors); ++i)
			mono_image_get_ctor_info (domain,
				mono_array_get (tb->ctors, MonoReflectionCtorBuilder*, i), assembly);
	}

	/* handle methods */
	if (tb->methods) {
		table = &assembly->tables [MONO_TABLE_METHOD];
		table->rows += mono_array_length (tb->methods);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (tb->methods); ++i)
			mono_image_get_method_info (
				mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i), assembly);
	}

	/* Do the same with properties etc.. */
	if (tb->events && mono_array_length (tb->events)) {
		table = &assembly->tables [MONO_TABLE_EVENT];
		table->rows += mono_array_length (tb->events);
		alloc_table (table, table->rows);
		table = &assembly->tables [MONO_TABLE_EVENTMAP];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_EVENT_MAP_SIZE;
		values [MONO_EVENT_MAP_PARENT] = tb->table_idx;
		values [MONO_EVENT_MAP_EVENTLIST] = assembly->tables [MONO_TABLE_EVENT].next_idx;
		for (i = 0; i < mono_array_length (tb->events); ++i)
			mono_image_get_event_info (
				mono_array_get (tb->events, MonoReflectionEventBuilder*, i), assembly);
	}
	if (tb->properties && mono_array_length (tb->properties)) {
		table = &assembly->tables [MONO_TABLE_PROPERTY];
		table->rows += mono_array_length (tb->properties);
		alloc_table (table, table->rows);
		table = &assembly->tables [MONO_TABLE_PROPERTYMAP];
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + table->rows * MONO_PROPERTY_MAP_SIZE;
		values [MONO_PROPERTY_MAP_PARENT] = tb->table_idx;
		values [MONO_PROPERTY_MAP_PROPERTY_LIST] = assembly->tables [MONO_TABLE_PROPERTY].next_idx;
		for (i = 0; i < mono_array_length (tb->properties); ++i)
			mono_image_get_property_info (
				mono_array_get (tb->properties, MonoReflectionPropertyBuilder*, i), assembly);
	}
	if (tb->subtypes) {
		MonoDynamicTable *ntable;
		
		ntable = &assembly->tables [MONO_TABLE_NESTEDCLASS];
		ntable->rows += mono_array_length (tb->subtypes);
		alloc_table (ntable, ntable->rows);
		values = ntable->values + ntable->next_idx * MONO_NESTED_CLASS_SIZE;

		for (i = 0; i < mono_array_length (tb->subtypes); ++i) {
			MonoReflectionTypeBuilder *subtype = mono_array_get (tb->subtypes, MonoReflectionTypeBuilder*, i);

			values [MONO_NESTED_CLASS_NESTED] = subtype->table_idx;
			values [MONO_NESTED_CLASS_ENCLOSING] = tb->table_idx;
			/*g_print ("nesting %s (%d) in %s (%d) (rows %d/%d)\n",
				mono_string_to_utf8 (subtype->name), subtype->table_idx,
				mono_string_to_utf8 (tb->name), tb->table_idx,
				ntable->next_idx, ntable->rows);*/
			values += MONO_NESTED_CLASS_SIZE;
			ntable->next_idx++;
		}
	}
}

static void
collect_types (GPtrArray *types, MonoReflectionTypeBuilder *type)
{
	int i;

	g_ptr_array_add (types, type);

	if (!type->subtypes)
		return;

	for (i = 0; i < mono_array_length (type->subtypes); ++i) {
		MonoReflectionTypeBuilder *subtype = mono_array_get (type->subtypes, MonoReflectionTypeBuilder*, i);
		collect_types (types, subtype);
	}
}

static gint
compare_types_by_table_idx (MonoReflectionTypeBuilder **type1, 
							MonoReflectionTypeBuilder **type2)
{
	if ((*type1)->table_idx < (*type2)->table_idx)
		return -1;
	else
		if ((*type1)->table_idx > (*type2)->table_idx)
			return 1;
	else
		return 0;
}

static void
params_add_cattrs (MonoDynamicImage *assembly, MonoArray *pinfo) {
	int i;

	if (!pinfo)
		return;
	for (i = 0; i < mono_array_length (pinfo); ++i) {
		MonoReflectionParamBuilder *pb;
		pb = mono_array_get (pinfo, MonoReflectionParamBuilder *, i);
		if (!pb)
			continue;
		mono_image_add_cattrs (assembly, pb->table_idx, CUSTOM_ATTR_PARAMDEF, pb->cattrs);
	}
}

static void
type_add_cattrs (MonoDynamicImage *assembly, MonoReflectionTypeBuilder *tb) {
	int i;
	
	mono_image_add_cattrs (assembly, tb->table_idx, CUSTOM_ATTR_TYPEDEF, tb->cattrs);
	if (tb->fields) {
		for (i = 0; i < mono_array_length (tb->fields); ++i) {
			MonoReflectionFieldBuilder* fb;
			fb = mono_array_get (tb->fields, MonoReflectionFieldBuilder*, i);
			mono_image_add_cattrs (assembly, fb->table_idx, CUSTOM_ATTR_FIELDDEF, fb->cattrs);
		}
	}
	if (tb->events) {
		for (i = 0; i < mono_array_length (tb->events); ++i) {
			MonoReflectionEventBuilder* eb;
			eb = mono_array_get (tb->events, MonoReflectionEventBuilder*, i);
			mono_image_add_cattrs (assembly, eb->table_idx, CUSTOM_ATTR_EVENT, eb->cattrs);
		}
	}
	if (tb->properties) {
		for (i = 0; i < mono_array_length (tb->properties); ++i) {
			MonoReflectionPropertyBuilder* pb;
			pb = mono_array_get (tb->properties, MonoReflectionPropertyBuilder*, i);
			mono_image_add_cattrs (assembly, pb->table_idx, CUSTOM_ATTR_PROPERTY, pb->cattrs);
		}
	}
	if (tb->ctors) {
		for (i = 0; i < mono_array_length (tb->ctors); ++i) {
			MonoReflectionCtorBuilder* cb;
			cb = mono_array_get (tb->ctors, MonoReflectionCtorBuilder*, i);
			mono_image_add_cattrs (assembly, cb->table_idx, CUSTOM_ATTR_METHODDEF, cb->cattrs);
			params_add_cattrs (assembly, cb->pinfo);
		}
	}

	if (tb->methods) {
		for (i = 0; i < mono_array_length (tb->methods); ++i) {
			MonoReflectionMethodBuilder* mb;
			mb = mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i);
			mono_image_add_cattrs (assembly, mb->table_idx, CUSTOM_ATTR_METHODDEF, mb->cattrs);
			params_add_cattrs (assembly, mb->pinfo);
		}
	}

	if (tb->subtypes) {
		for (i = 0; i < mono_array_length (tb->subtypes); ++i)
			type_add_cattrs (assembly, mono_array_get (tb->subtypes, MonoReflectionTypeBuilder*, i));
	}
}

static void
module_add_cattrs (MonoDynamicImage *assembly, MonoReflectionModuleBuilder *mb) {
	int i;
	
	mono_image_add_cattrs (assembly, mb->table_idx, CUSTOM_ATTR_MODULE, mb->cattrs);
	
	/* no types in the module */
	if (!mb->types)
		return;
	
	for (i = 0; i < mono_array_length (mb->types); ++i)
		type_add_cattrs (assembly, mono_array_get (mb->types, MonoReflectionTypeBuilder*, i));
}

static void
mono_image_fill_module_table (MonoDomain *domain, MonoReflectionModuleBuilder *mb, MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	int i;
	char *name;

	table = &assembly->tables [MONO_TABLE_MODULE];
	mb->table_idx = table->next_idx ++;
	name = mono_string_to_utf8 (mb->module.name);
	table->values [mb->table_idx * MONO_MODULE_SIZE + MONO_MODULE_NAME] = string_heap_insert (&assembly->sheap, name);
	g_free (name);
	i = mono_image_add_stream_data (&assembly->guid, mono_array_addr (mb->guid, char, 0), 16);
	i /= 16;
	++i;
	table->values [mb->table_idx * MONO_MODULE_SIZE + MONO_MODULE_MVID] = i;
	table->values [mb->table_idx * MONO_MODULE_SIZE + MONO_MODULE_ENC] = 0;
	table->values [mb->table_idx * MONO_MODULE_SIZE + MONO_MODULE_ENCBASE] = 0;
}

#define align_pointer(base,p)\
	do {\
		guint32 __diff = (unsigned char*)(p)-(unsigned char*)(base);\
		if (__diff & 3)\
			(p) += 4 - (__diff & 3);\
	} while (0)

static int
compare_semantics (const void *a, const void *b)
{
	const guint32 *a_values = a;
	const guint32 *b_values = b;
	int assoc = a_values [MONO_METHOD_SEMA_ASSOCIATION] - b_values [MONO_METHOD_SEMA_ASSOCIATION];
	if (assoc)
		return assoc;
	return a_values [MONO_METHOD_SEMA_SEMANTICS] - b_values [MONO_METHOD_SEMA_SEMANTICS];
}

static int
compare_custom_attrs (const void *a, const void *b)
{
	const guint32 *a_values = a;
	const guint32 *b_values = b;

	return a_values [MONO_CUSTOM_ATTR_PARENT] - b_values [MONO_CUSTOM_ATTR_PARENT];
}

static int
compare_field_marshal (const void *a, const void *b)
{
	const guint32 *a_values = a;
	const guint32 *b_values = b;

	return a_values [MONO_FIELD_MARSHAL_PARENT] - b_values [MONO_FIELD_MARSHAL_PARENT];
}

static int
compare_nested (const void *a, const void *b)
{
	const guint32 *a_values = a;
	const guint32 *b_values = b;

	return a_values [MONO_NESTED_CLASS_NESTED] - b_values [MONO_NESTED_CLASS_NESTED];
}

/*
 * build_compressed_metadata() fills in the blob of data that represents the 
 * raw metadata as it will be saved in the PE file. The five streams are output 
 * and the metadata tables are comnpressed from the guint32 array representation, 
 * to the compressed on-disk format.
 */
static void
build_compressed_metadata (MonoDynamicImage *assembly)
{
	MonoDynamicTable *table;
	int i;
	guint64 valid_mask = 0;
	guint64 sorted_mask;
	guint32 heapt_size = 0;
	guint32 meta_size = 256; /* allow for header and other stuff */
	guint32 table_offset;
	guint32 ntables = 0;
	guint64 *int64val;
	guint32 *int32val;
	guint16 *int16val;
	MonoImage *meta;
	unsigned char *p;

	struct StreamDesc {
		const char *name;
		MonoDynamicStream *stream;
	} stream_desc [5];

	stream_desc[0].name  = "#~"; stream_desc[0].stream = &assembly->tstream;
	stream_desc[1].name  = "#Strings"; stream_desc[1].stream = &assembly->sheap;
	stream_desc[2].name  = "#US"; stream_desc[2].stream = &assembly->us;
	stream_desc[3].name  = "#Blob"; stream_desc[3].stream = &assembly->blob;
	stream_desc[4].name  = "#GUID"; stream_desc[4].stream = &assembly->guid;
	
	/* tables that are sorted */
	sorted_mask = ((guint64)1 << MONO_TABLE_CONSTANT) | ((guint64)1 << MONO_TABLE_FIELDMARSHAL)
		| ((guint64)1 << MONO_TABLE_METHODSEMANTICS) | ((guint64)1 << MONO_TABLE_CLASSLAYOUT)
		| ((guint64)1 << MONO_TABLE_FIELDLAYOUT) | ((guint64)1 << MONO_TABLE_FIELDRVA)
		| ((guint64)1 << MONO_TABLE_IMPLMAP) | ((guint64)1 << MONO_TABLE_NESTEDCLASS)
		| ((guint64)1 << MONO_TABLE_METHODIMPL) | ((guint64)1 << MONO_TABLE_CUSTOMATTRIBUTE)
		| ((guint64)1 << MONO_TABLE_DECLSECURITY);
	
	/* Compute table sizes */
	/* the MonoImage has already been created in mono_image_basic_init() */
	meta = &assembly->image;
	
	/* Setup the info used by compute_sizes () */
	meta->idx_blob_wide = assembly->blob.index >= 65536 ? 1 : 0;
	meta->idx_guid_wide = assembly->guid.index >= 65536 ? 1 : 0;
	meta->idx_string_wide = assembly->sheap.index >= 65536 ? 1 : 0;

	meta_size += assembly->blob.index;
	meta_size += assembly->guid.index;
	meta_size += assembly->sheap.index;
	meta_size += assembly->us.index;

	for (i=0; i < 64; ++i)
		meta->tables [i].rows = assembly->tables [i].rows;
	
	for (i = 0; i < 64; i++){
		if (meta->tables [i].rows == 0)
			continue;
		valid_mask |= (guint64)1 << i;
		ntables ++;
		meta->tables [i].row_size = mono_metadata_compute_size (
			meta, i, &meta->tables [i].size_bitfield);
		heapt_size += meta->tables [i].row_size * meta->tables [i].rows;
	}
	heapt_size += 24; /* #~ header size */
	heapt_size += ntables * 4;
	meta_size += heapt_size;
	meta->raw_metadata = g_malloc0 (meta_size);
	p = meta->raw_metadata;
	/* the metadata signature */
	*p++ = 'B'; *p++ = 'S'; *p++ = 'J'; *p++ = 'B';
	/* version numbers and 4 bytes reserved */
	int16val = (guint16*)p;
	*int16val++ = GUINT16_TO_LE (1);
	*int16val = GUINT16_TO_LE (1);
	p += 8;
	/* version string */
	int32val = (guint32*)p;
	*int32val = GUINT32_TO_LE ((strlen (meta->version) + 3) & (~3)); /* needs to be multiple of 4 */
	p += 4;
	memcpy (p, meta->version, GUINT32_FROM_LE (*int32val));
	p += GUINT32_FROM_LE (*int32val);
	align_pointer (meta->raw_metadata, p);
	int16val = (guint16*)p;
	*int16val++ = GUINT16_TO_LE (0); /* flags must be 0 */
	*int16val = GUINT16_TO_LE (5); /* number of streams */
	p += 4;

	/*
	 * write the stream info.
	 */
	table_offset = (p - (unsigned char*)meta->raw_metadata) + 5 * 8 + 40; /* room needed for stream headers */
	table_offset += 3; table_offset &= ~3;

	assembly->tstream.index = heapt_size;
	for (i = 0; i < 5; ++i) {
		int32val = (guint32*)p;
		stream_desc [i].stream->offset = table_offset;
		*int32val++ = GUINT32_TO_LE (table_offset);
		*int32val = GUINT32_TO_LE (stream_desc [i].stream->index);
		table_offset += GUINT32_FROM_LE (*int32val);
		table_offset += 3; table_offset &= ~3;
		p += 8;
		strcpy (p, stream_desc [i].name);
		p += strlen (stream_desc [i].name) + 1;
		align_pointer (meta->raw_metadata, p);
	}
	/* 
	 * now copy the data, the table stream header and contents goes first.
	 */
	g_assert ((p - (unsigned char*)meta->raw_metadata) < assembly->tstream.offset);
	p = meta->raw_metadata + assembly->tstream.offset;
	int32val = (guint32*)p;
	*int32val = GUINT32_TO_LE (0); /* reserved */
	p += 4;
	*p++ = 1; /* version */
	*p++ = 0;
	if (meta->idx_string_wide)
		*p |= 0x01;
	if (meta->idx_guid_wide)
		*p |= 0x02;
	if (meta->idx_blob_wide)
		*p |= 0x04;
	++p;
	*p++ = 0; /* reserved */
	int64val = (guint64*)p;
	*int64val++ = GUINT64_TO_LE (valid_mask);
	*int64val++ = GUINT64_TO_LE (valid_mask & sorted_mask); /* bitvector of sorted tables  */
	p += 16;
	int32val = (guint32*)p;
	for (i = 0; i < 64; i++){
		if (meta->tables [i].rows == 0)
			continue;
		*int32val++ = GUINT32_TO_LE (meta->tables [i].rows);
	}
	p = (unsigned char*)int32val;

	/* sort the tables that still need sorting */
	table = &assembly->tables [MONO_TABLE_METHODSEMANTICS];
	if (table->rows)
		qsort (table->values + MONO_METHOD_SEMA_SIZE, table->rows, sizeof (guint32) * MONO_METHOD_SEMA_SIZE, compare_semantics);
	table = &assembly->tables [MONO_TABLE_CUSTOMATTRIBUTE];
	if (table->rows)
		qsort (table->values + MONO_CUSTOM_ATTR_SIZE, table->rows, sizeof (guint32) * MONO_CUSTOM_ATTR_SIZE, compare_custom_attrs);
	table = &assembly->tables [MONO_TABLE_FIELDMARSHAL];
	if (table->rows)
		qsort (table->values + MONO_FIELD_MARSHAL_SIZE, table->rows, sizeof (guint32) * MONO_FIELD_MARSHAL_SIZE, compare_field_marshal);
	table = &assembly->tables [MONO_TABLE_NESTEDCLASS];
	if (table->rows)
		qsort (table->values + MONO_NESTED_CLASS_SIZE, table->rows, sizeof (guint32) * MONO_NESTED_CLASS_SIZE, compare_nested);

	/* compress the tables */
	for (i = 0; i < 64; i++){
		int row, col;
		guint32 *values;
		guint32 bitfield = meta->tables [i].size_bitfield;
		if (!meta->tables [i].rows)
			continue;
		if (assembly->tables [i].columns != mono_metadata_table_count (bitfield))
			g_error ("col count mismatch in %d: %d %d", i, assembly->tables [i].columns, mono_metadata_table_count (bitfield));
		meta->tables [i].base = p;
		for (row = 1; row <= meta->tables [i].rows; ++row) {
			values = assembly->tables [i].values + row * assembly->tables [i].columns;
			for (col = 0; col < assembly->tables [i].columns; ++col) {
				switch (mono_metadata_table_size (bitfield, col)) {
				case 1:
					*p++ = values [col];
					break;
				case 2:
					*p++ = values [col] & 0xff;
					*p++ = (values [col] >> 8) & 0xff;
					break;
				case 4:
					*p++ = values [col] & 0xff;
					*p++ = (values [col] >> 8) & 0xff;
					*p++ = (values [col] >> 16) & 0xff;
					*p++ = (values [col] >> 24) & 0xff;
					break;
				default:
					g_assert_not_reached ();
				}
			}
		}
		g_assert ((p - (const unsigned char*)meta->tables [i].base) == (meta->tables [i].rows * meta->tables [i].row_size));
	}
	
	g_assert (assembly->guid.offset + assembly->guid.index < meta_size);
	memcpy (meta->raw_metadata + assembly->sheap.offset, assembly->sheap.data, assembly->sheap.index);
	memcpy (meta->raw_metadata + assembly->us.offset, assembly->us.data, assembly->us.index);
	memcpy (meta->raw_metadata + assembly->blob.offset, assembly->blob.data, assembly->blob.index);
	memcpy (meta->raw_metadata + assembly->guid.offset, assembly->guid.data, assembly->guid.index);

	assembly->meta_size = assembly->guid.offset + assembly->guid.index;
}

/*
 * Some tables in metadata need to be sorted according to some criteria, but
 * when methods and fields are first created with reflection, they may be assigned a token
 * that doesn't correspond to the final token they will get assigned after the sorting.
 * ILGenerator.cs keeps a fixup table that maps the position of tokens in the IL code stream
 * with the reflection objects that represent them. Once all the tables are set up, the 
 * reflection objects will contains the correct table index. fixup_method() will fixup the
 * tokens for the method with ILGenerator @ilgen.
 */
static void
fixup_method (MonoReflectionILGen *ilgen, gpointer value, MonoDynamicImage *assembly) {
	guint32 code_idx = GPOINTER_TO_UINT (value);
	MonoReflectionILTokenInfo *iltoken;
	MonoReflectionFieldBuilder *field;
	MonoReflectionCtorBuilder *ctor;
	MonoReflectionMethodBuilder *method;
	MonoReflectionTypeBuilder *tb;
	MonoReflectionArrayMethod *am;
	guint32 i, idx;
	unsigned char *target;

	for (i = 0; i < ilgen->num_token_fixups; ++i) {
		iltoken = (MonoReflectionILTokenInfo *)mono_array_addr_with_size (ilgen->token_fixups, sizeof (MonoReflectionILTokenInfo), i);
		target = assembly->code.data + code_idx + iltoken->code_pos;
		switch (target [3]) {
		case MONO_TABLE_FIELD:
			if (!strcmp (iltoken->member->vtable->klass->name, "FieldBuilder")) {
				field = (MonoReflectionFieldBuilder *)iltoken->member;
				idx = field->table_idx;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "MonoField")) {
				MonoClassField *f = ((MonoReflectionField*)iltoken->member)->field;
				idx = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->field_to_table_idx, f));
			} else {
				g_assert_not_reached ();
			}
			break;
		case MONO_TABLE_METHOD:
			if (!strcmp (iltoken->member->vtable->klass->name, "MethodBuilder")) {
				method = (MonoReflectionMethodBuilder *)iltoken->member;
				idx = method->table_idx;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "ConstructorBuilder")) {
				ctor = (MonoReflectionCtorBuilder *)iltoken->member;
				idx = ctor->table_idx;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "MonoMethod")) {
				MonoMethod *m = ((MonoReflectionMethod*)iltoken->member)->method;
				idx = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->method_to_table_idx, m));
			} else {
				g_assert_not_reached ();
			}
			break;
		case MONO_TABLE_TYPEDEF:
			if (strcmp (iltoken->member->vtable->klass->name, "TypeBuilder"))
				g_assert_not_reached ();
			tb = (MonoReflectionTypeBuilder *)iltoken->member;
			idx = tb->table_idx;
			break;
		case MONO_TABLE_MEMBERREF:
			if (!strcmp (iltoken->member->vtable->klass->name, "MonoArrayMethod")) {
				am = (MonoReflectionArrayMethod*)iltoken->member;
				idx = am->table_idx;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "MonoMethod") ||
				   !strcmp (iltoken->member->vtable->klass->name, "MonoCMethod")) {
				MonoMethod *m = ((MonoReflectionMethod*)iltoken->member)->method;
				g_assert (m->klass->generic_inst);
				continue;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "MonoInflatedMethod") ||
				   !strcmp (iltoken->member->vtable->klass->name, "MonoInflatedCtor") ||
				   !strcmp (iltoken->member->vtable->klass->name, "MonoInflatedField")) {
				continue;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "FieldBuilder")) {
				continue;
			} else {
				g_assert_not_reached ();
			}
			break;
		case MONO_TABLE_METHODSPEC:
			if (!strcmp (iltoken->member->vtable->klass->name, "MonoMethod")) {
				MonoMethod *m = ((MonoReflectionMethod*)iltoken->member)->method;
				g_assert (m->signature->generic_param_count);
				continue;
			} else if (!strcmp (iltoken->member->vtable->klass->name, "MonoInflatedMethod") ||
				   !strcmp (iltoken->member->vtable->klass->name, "MonoInflatedCtor")) {
				continue;
			} else {
				g_assert_not_reached ();
			}
			break;
		default:
			g_error ("got unexpected table 0x%02x in fixup", target [3]);
		}
		target [0] = idx & 0xff;
		target [1] = (idx >> 8) & 0xff;
		target [2] = (idx >> 16) & 0xff;
	}
}

static void
assembly_add_resource (MonoDynamicImage *assembly, MonoReflectionResource *rsrc)
{
	MonoDynamicTable *table;
	guint32 *values;
	char blob_size [6];
	guchar hash [20];
	char *b = blob_size;
	char *name, *sname;
	guint32 idx, offset;

	if (rsrc->filename) {
		name = mono_string_to_utf8 (rsrc->filename);
		sname = g_path_get_basename (name);
	
		table = &assembly->tables [MONO_TABLE_FILE];
		table->rows++;
		alloc_table (table, table->rows);
		values = table->values + table->next_idx * MONO_FILE_SIZE;
		values [MONO_FILE_FLAGS] = FILE_CONTAINS_NO_METADATA;
		values [MONO_FILE_NAME] = string_heap_insert (&assembly->sheap, sname);
		g_free (sname);

		mono_sha1_get_digest_from_file (name, hash);
		mono_metadata_encode_value (20, b, &b);
		values [MONO_FILE_HASH_VALUE] = mono_image_add_stream_data (&assembly->blob, blob_size, b-blob_size);
		mono_image_add_stream_data (&assembly->blob, hash, 20);
		g_free (name);
		idx = table->next_idx++;
		idx = IMPLEMENTATION_FILE | (idx << IMPLEMENTATION_BITS);
		offset = 0;
	} else {
		char sizebuf [4];
		offset = mono_array_length (rsrc->data);
		sizebuf [0] = offset; sizebuf [1] = offset >> 8;
		sizebuf [2] = offset >> 16; sizebuf [3] = offset >> 24;
		offset = mono_image_add_stream_data (&assembly->resources, sizebuf, 4);
		mono_image_add_stream_data (&assembly->resources, mono_array_addr (rsrc->data, char, 0), mono_array_length (rsrc->data));
		idx = 0;
	}

	table = &assembly->tables [MONO_TABLE_MANIFESTRESOURCE];
	table->rows++;
	alloc_table (table, table->rows);
	values = table->values + table->next_idx * MONO_MANIFEST_SIZE;
	values [MONO_MANIFEST_OFFSET] = offset;
	values [MONO_MANIFEST_FLAGS] = rsrc->attrs;
	name = mono_string_to_utf8 (rsrc->name);
	values [MONO_MANIFEST_NAME] = string_heap_insert (&assembly->sheap, name);
	g_free (name);
	values [MONO_MANIFEST_IMPLEMENTATION] = idx;
	table->next_idx++;
}

static void
set_version_from_string (MonoString *version, guint32 *values)
{
	gchar *ver, *p, *str;
	guint32 i;
	
	values [MONO_ASSEMBLY_MAJOR_VERSION] = 0;
	values [MONO_ASSEMBLY_MINOR_VERSION] = 0;
	values [MONO_ASSEMBLY_REV_NUMBER] = 0;
	values [MONO_ASSEMBLY_BUILD_NUMBER] = 0;
	if (!version)
		return;
	ver = str = mono_string_to_utf8 (version);
	for (i = 0; i < 4; ++i) {
		values [MONO_ASSEMBLY_MAJOR_VERSION + i] = strtol (ver, &p, 10);
		switch (*p) {
		case '.':
			p++;
			break;
		case '*':
			/* handle Revision and Build */
			p++;
			break;
		}
		ver = p;
	}
	g_free (str);
}

static guint32
load_public_key (MonoArray *pkey, MonoDynamicImage *assembly) {
	gsize len;
	guint32 token = 0;
	char blob_size [6];
	char *b = blob_size;

	if (!pkey)
		return token;

	len = mono_array_length (pkey);
	mono_metadata_encode_value (len, b, &b);
	token = mono_image_add_stream_data (&assembly->blob, blob_size, b - blob_size);
	mono_image_add_stream_data (&assembly->blob, mono_array_addr (pkey, guint8, 0), len);

	/* need to get the actual value from the key type... */
	assembly->strong_name_size = 128;
	assembly->strong_name = g_malloc0 (assembly->strong_name_size);

	return token;
}

/*
 * mono_image_build_metadata() will fill the info in all the needed metadata tables
 * for the modulebuilder @moduleb.
 * At the end of the process, method and field tokens are fixed up and the 
 * on-disk compressed metadata representation is created.
 */
void
mono_image_build_metadata (MonoReflectionModuleBuilder *moduleb)
{
	MonoDynamicTable *table;
	MonoDynamicImage *assembly;
	MonoReflectionAssemblyBuilder *assemblyb;
	MonoDomain *domain;
	guint32 *values;
	char *name;
	int i;

	assemblyb = moduleb->assemblyb;
	assembly = moduleb->dynamic_image;
	domain = mono_object_domain (assemblyb);

	if (assembly->text_rva)
		return;

	assembly->text_rva = START_TEXT_RVA;

	if (moduleb->is_main) {
		/* Emit the manifest */
		table = &assembly->tables [MONO_TABLE_ASSEMBLY];
		alloc_table (table, 1);
		values = table->values + MONO_ASSEMBLY_SIZE;
		values [MONO_ASSEMBLY_HASH_ALG] = assemblyb->algid? assemblyb->algid: ASSEMBLY_HASH_SHA1;
		name = mono_string_to_utf8 (assemblyb->name);
		values [MONO_ASSEMBLY_NAME] = string_heap_insert (&assembly->sheap, name);
		g_free (name);
		if (assemblyb->culture) {
			name = mono_string_to_utf8 (assemblyb->culture);
			values [MONO_ASSEMBLY_CULTURE] = string_heap_insert (&assembly->sheap, name);
			g_free (name);
		} else {
			values [MONO_ASSEMBLY_CULTURE] = string_heap_insert (&assembly->sheap, "");
		}
		values [MONO_ASSEMBLY_PUBLIC_KEY] = load_public_key (assemblyb->public_key, assembly);
		values [MONO_ASSEMBLY_FLAGS] = assemblyb->flags;
		set_version_from_string (assemblyb->version, values);
	}

	table = &assembly->tables [MONO_TABLE_TYPEDEF];
	table->rows = 1; /* .<Module> */
	table->next_idx++;
	alloc_table (table, table->rows);
	/*
	 * Set the first entry.
	 */
	values = table->values + table->columns;
	values [MONO_TYPEDEF_FLAGS] = 0;
	values [MONO_TYPEDEF_NAME] = string_heap_insert (&assembly->sheap, "<Module>") ;
	values [MONO_TYPEDEF_NAMESPACE] = string_heap_insert (&assembly->sheap, "") ;
	values [MONO_TYPEDEF_EXTENDS] = 0;
	values [MONO_TYPEDEF_FIELD_LIST] = 1;
	values [MONO_TYPEDEF_METHOD_LIST] = 1;

	/* 
	 * handle global methods 
	 * FIXME: test what to do when global methods are defined in multiple modules.
	 */
	if (moduleb->global_methods) {
		table = &assembly->tables [MONO_TABLE_METHOD];
		table->rows += mono_array_length (moduleb->global_methods);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (moduleb->global_methods); ++i)
			mono_image_get_method_info (
										mono_array_get (moduleb->global_methods, MonoReflectionMethodBuilder*, i), assembly);
	}
	if (moduleb->global_fields) {
		table = &assembly->tables [MONO_TABLE_FIELD];
		table->rows += mono_array_length (moduleb->global_fields);
		alloc_table (table, table->rows);
		for (i = 0; i < mono_array_length (moduleb->global_fields); ++i)
			mono_image_get_field_info (
									   mono_array_get (moduleb->global_fields, MonoReflectionFieldBuilder*, i), assembly);
	}

	table = &assembly->tables [MONO_TABLE_MODULE];
	alloc_table (table, 1);
	mono_image_fill_module_table (domain, moduleb, assembly);

	/* Emit types */
	{
		/* Collect all types into a list sorted by their table_idx */
		GPtrArray *types = g_ptr_array_new ();

		if (moduleb->types)
			for (i = 0; i < mono_array_length (moduleb->types); ++i) {
				MonoReflectionTypeBuilder *type = mono_array_get (moduleb->types, MonoReflectionTypeBuilder*, i);
				collect_types (types, type);
			}

		g_ptr_array_sort (types, (GCompareFunc)compare_types_by_table_idx);
		table = &assembly->tables [MONO_TABLE_TYPEDEF];
		table->rows += types->len;
		alloc_table (table, table->rows);

		for (i = 0; i < types->len; ++i) {
			MonoReflectionTypeBuilder *type = g_ptr_array_index (types, i);
			mono_image_get_type_info (domain, type, assembly);
		}
		g_ptr_array_free (types, TRUE);
	}

	/* 
	 * table->rows is already set above and in mono_image_fill_module_table.
	 */
	/* add all the custom attributes at the end, once all the indexes are stable */
	mono_image_add_cattrs (assembly, 1, CUSTOM_ATTR_ASSEMBLY, assemblyb->cattrs);

	module_add_cattrs (assembly, moduleb);

	/* fixup tokens */
	mono_g_hash_table_foreach (assembly->token_fixups, (GHFunc)fixup_method, assembly);
}

/*
 * mono_image_insert_string:
 * @module: module builder object
 * @str: a string
 *
 * Insert @str into the user string stream of @module.
 */
guint32
mono_image_insert_string (MonoReflectionModuleBuilder *module, MonoString *str)
{
	MonoDynamicImage *assembly;
	guint32 idx;
	char buf [16];
	char *b = buf;
	
	MONO_ARCH_SAVE_REGS;

	if (!module->dynamic_image)
		mono_image_module_basic_init (module);

	assembly = module->dynamic_image;
	
	if (assembly->save) {
		mono_metadata_encode_value (1 | (str->length * 2), b, &b);
		idx = mono_image_add_stream_data (&assembly->us, buf, b-buf);
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	{
		char *swapped = g_malloc (2 * mono_string_length (str));
		const char *p = (const char*)mono_string_chars (str);

		swap_with_size (swapped, p, 2, mono_string_length (str));
		mono_image_add_stream_data (&assembly->us, swapped, str->length * 2);
		g_free (swapped);
	}
#else
	mono_image_add_stream_data (&assembly->us, (const char*)mono_string_chars (str), str->length * 2);
#endif
	mono_image_add_stream_data (&assembly->us, "", 1);
	}
	else
		idx = assembly->us.index ++;

	mono_g_hash_table_insert (assembly->tokens, 
							  GUINT_TO_POINTER (MONO_TOKEN_STRING | idx), str);

	return MONO_TOKEN_STRING | idx;
}

/*
 * mono_image_create_token:
 * @assembly: a dynamic assembly
 * @obj:
 *
 * Get a token to insert in the IL code stream for the given MemberInfo.
 * @obj can be one of:
 * 	ConstructorBuilder
 * 	MethodBuilder
 * 	FieldBuilder
 * 	MonoCMethod
 * 	MonoMethod
 * 	MonoField
 * 	MonoType
 * 	TypeBuilder
 */
guint32
mono_image_create_token (MonoDynamicImage *assembly, MonoObject *obj)
{
	MonoClass *klass;
	guint32 token;

	klass = obj->vtable->klass;
	if (strcmp (klass->name, "MethodBuilder") == 0) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder *)obj;

		if (((MonoReflectionTypeBuilder*)mb->type)->module->dynamic_image == assembly)
			token = mb->table_idx | MONO_TOKEN_METHOD_DEF;
		else
			token = mono_image_get_methodbuilder_token (assembly, mb);
		/*g_print ("got token 0x%08x for %s\n", token, mono_string_to_utf8 (mb->name));*/
	}
	else if (strcmp (klass->name, "ConstructorBuilder") == 0) {
		MonoReflectionCtorBuilder *mb = (MonoReflectionCtorBuilder *)obj;

		if (((MonoReflectionTypeBuilder*)mb->type)->module->dynamic_image == assembly)
			token = mb->table_idx | MONO_TOKEN_METHOD_DEF;
		else
			token = mono_image_get_ctorbuilder_token (assembly, mb);
		/*g_print ("got token 0x%08x for %s\n", token, mono_string_to_utf8 (mb->name));*/
	}
	else if (strcmp (klass->name, "FieldBuilder") == 0) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder *)obj;
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)fb->typeb;
		if (tb->generic_params) {
			token = mono_image_get_generic_field_token (assembly, fb);
		} else {
			token = fb->table_idx | MONO_TOKEN_FIELD_DEF;
		}
	}
	else if (strcmp (klass->name, "TypeBuilder") == 0) {
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)obj;
		token = tb->table_idx | MONO_TOKEN_TYPE_DEF;
	}
	else if (strcmp (klass->name, "MonoType") == 0) {
		MonoReflectionType *tb = (MonoReflectionType *)obj;
		token = mono_metadata_token_from_dor (
			mono_image_typedef_or_ref (assembly, tb->type));
	}
	else if (strcmp (klass->name, "MonoCMethod") == 0 ||
			strcmp (klass->name, "MonoMethod") == 0) {
		MonoReflectionMethod *m = (MonoReflectionMethod *)obj;
		if (m->method->signature->generic_param_count) {
			g_assert_not_reached ();
			token = mono_image_get_methodspec_token (assembly, m->method, NULL);
		} else if ((m->method->klass->image == &assembly->image) &&
			 !m->method->klass->generic_inst) {
			static guint32 method_table_idx = 0xffffff;
			/*
			 * Each token should have a unique index, but the indexes are
			 * assigned by managed code, so we don't know about them. An
			 * easy solution is to count backwards...
			 */
			method_table_idx --;
			token = MONO_TOKEN_METHOD_DEF | method_table_idx;
		} else
			token = mono_image_get_methodref_token (assembly, m->method);
		/*g_print ("got token 0x%08x for %s\n", token, m->method->name);*/
	}
	else if (strcmp (klass->name, "MonoInflatedMethod") == 0 ||
		 strcmp (klass->name, "MonoInflatedCtor") == 0) {
		MonoReflectionInflatedMethod *m = (MonoReflectionInflatedMethod *)obj;
		token = mono_image_get_methodspec_token (assembly, m->rmethod.method, m->ginst);
	}
	else if (strcmp (klass->name, "MonoInflatedField") == 0) {
		MonoReflectionInflatedField *f = (MonoReflectionInflatedField *)obj;
		token = mono_image_get_inflated_field_token (assembly, f);
	}
	else if (strcmp (klass->name, "MonoField") == 0) {
		MonoReflectionField *f = (MonoReflectionField *)obj;
		if (f->klass->image == &assembly->image) {
			static guint32 field_table_idx = 0xffffff;
			field_table_idx --;
			token = MONO_TOKEN_FIELD_DEF | field_table_idx;
		} else
			token = mono_image_get_fieldref_token (assembly, f->field, f->klass);
		/*g_print ("got token 0x%08x for %s\n", token, f->field->name);*/
	}
	else if (strcmp (klass->name, "MonoArrayMethod") == 0) {
		MonoReflectionArrayMethod *m = (MonoReflectionArrayMethod *)obj;
		token = mono_image_get_array_token (assembly, m);
	}
	else if (strcmp (klass->name, "SignatureHelper") == 0) {
		MonoReflectionSigHelper *s = (MonoReflectionSigHelper*)obj;
		token = MONO_TOKEN_SIGNATURE | mono_image_get_sighelper_token (assembly, s);
	}
	else
		g_error ("requested token for %s\n", klass->name);

	mono_g_hash_table_insert (assembly->tokens, GUINT_TO_POINTER (token),
							  obj);

	return token;
}

typedef struct {
	guint32 import_lookup_table;
	guint32 timestamp;
	guint32 forwarder;
	guint32 name_rva;
	guint32 import_address_table_rva;
} MonoIDT;

typedef struct {
	guint32 name_rva;
	guint32 flags;
} MonoILT;

static void register_assembly (MonoDomain *domain, MonoReflectionAssembly *res, MonoAssembly *assembly);

static MonoDynamicImage*
create_dynamic_mono_image (MonoDynamicAssembly *assembly, 
						   char *assembly_name, char *module_name)
{
	static const guchar entrycode [16] = {0xff, 0x25, 0};
	MonoDynamicImage *image;
	int i;

	/*
	 * We need to use the current ms version or the ms runtime it won't find
	 * the support dlls. D'oh!
	 * const char *version = "mono-" VERSION;
	 */
	const char *version = "v1.0.3705";

#if HAVE_BOEHM_GC
	image = GC_MALLOC (sizeof (MonoDynamicImage));
#else
	image = g_new0 (MonoDynamicImage, 1);
#endif

	/* keep in sync with image.c */
	image->image.name = assembly_name;
	image->image.assembly_name = image->image.name; /* they may be different */
	image->image.module_name = module_name;
	image->image.version = g_strdup (version);

	image->image.references = g_new0 (MonoAssembly*, 1);
	image->image.references [0] = NULL;

	mono_image_init (&image->image);

	image->token_fixups = mono_g_hash_table_new (NULL, NULL);
	image->method_to_table_idx = mono_g_hash_table_new (NULL, NULL);
	image->field_to_table_idx = mono_g_hash_table_new (NULL, NULL);
	image->method_aux_hash = mono_g_hash_table_new (NULL, NULL);
	image->handleref = g_hash_table_new (NULL, NULL);
	image->tokens = mono_g_hash_table_new (NULL, NULL);
	image->typeref = g_hash_table_new ((GHashFunc)mono_metadata_type_hash, (GCompareFunc)mono_metadata_type_equal);
	image->blob_cache = mono_g_hash_table_new ((GHashFunc)mono_blob_entry_hash, (GCompareFunc)mono_blob_entry_equal);

	string_heap_init (&image->sheap);
	mono_image_add_stream_data (&image->us, "", 1);
	add_to_blob_cached (image, (char*) "", 1, NULL, 0);
	/* import tables... */
	mono_image_add_stream_data (&image->code, entrycode, sizeof (entrycode));
	image->iat_offset = mono_image_add_stream_zero (&image->code, 8); /* two IAT entries */
	image->idt_offset = mono_image_add_stream_zero (&image->code, 2 * sizeof (MonoIDT)); /* two IDT entries */
	mono_image_add_stream_zero (&image->code, 2); /* flags for name entry */
	image->imp_names_offset = mono_image_add_stream_data (&image->code, "_CorExeMain", 12);
	mono_image_add_stream_data (&image->code, "mscoree.dll", 12);
	image->ilt_offset = mono_image_add_stream_zero (&image->code, 8); /* two ILT entries */
	stream_data_align (&image->code);

	image->cli_header_offset = mono_image_add_stream_zero (&image->code, sizeof (MonoCLIHeader));

	for (i=0; i < 64; ++i) {
		image->tables [i].next_idx = 1;
		image->tables [i].columns = table_sizes [i];
	}

	image->image.assembly = (MonoAssembly*)assembly;
	image->run = assembly->run;
	image->save = assembly->save;

	return image;
}

/*
 * mono_image_basic_init:
 * @assembly: an assembly builder object
 *
 * Create the MonoImage that represents the assembly builder and setup some
 * of the helper hash table and the basic metadata streams.
 */
void
mono_image_basic_init (MonoReflectionAssemblyBuilder *assemblyb)
{
	MonoDynamicAssembly *assembly;
	MonoDynamicImage *image;
	
	MONO_ARCH_SAVE_REGS;

	if (assemblyb->dynamic_assembly)
		return;

#if HAVE_BOEHM_GC
	assembly = assemblyb->dynamic_assembly = GC_MALLOC (sizeof (MonoDynamicImage));
#else
	assembly = assemblyb->dynamic_assembly = g_new0 (MonoDynamicImage, 1);
#endif

	assembly->assembly.dynamic = TRUE;
	assemblyb->assembly.assembly = (MonoAssembly*)assembly;
	assembly->assembly.basedir = mono_string_to_utf8 (assemblyb->dir);
	if (assemblyb->culture)
		assembly->assembly.aname.culture = mono_string_to_utf8 (assemblyb->culture);
	else
		assembly->assembly.aname.culture = g_strdup ("");

	assembly->run = assemblyb->access != 2;
	assembly->save = assemblyb->access != 1;

	image = create_dynamic_mono_image (assembly, mono_string_to_utf8 (assemblyb->name), g_strdup ("RefEmit_YouForgotToDefineAModule"));
	assembly->assembly.aname.name = image->image.name;
	assembly->assembly.image = &image->image;

	register_assembly (mono_object_domain (assemblyb), &assemblyb->assembly, &assembly->assembly);
	mono_assembly_invoke_load_hook ((MonoAssembly*)assembly);
}

static int
calc_section_size (MonoDynamicImage *assembly)
{
	int nsections = 0;

	/* alignment constraints */
	assembly->code.index += 3;
	assembly->code.index &= ~3;
	assembly->meta_size += 3;
	assembly->meta_size &= ~3;
	assembly->resources.index += 3;
	assembly->resources.index &= ~3;

	assembly->sections [MONO_SECTION_TEXT].size = assembly->meta_size + assembly->code.index + assembly->resources.index + assembly->strong_name_size;
	assembly->sections [MONO_SECTION_TEXT].attrs = SECT_FLAGS_HAS_CODE | SECT_FLAGS_MEM_EXECUTE | SECT_FLAGS_MEM_READ;
	nsections++;

	assembly->sections [MONO_SECTION_RELOC].size = 12;
	assembly->sections [MONO_SECTION_RELOC].attrs = SECT_FLAGS_MEM_READ | SECT_FLAGS_MEM_DISCARDABLE | SECT_FLAGS_HAS_INITIALIZED_DATA;
	nsections++;

	return nsections;
}

/*
 * mono_image_create_pefile:
 * @mb: a module builder object
 * 
 * This function creates the PE-COFF header, the image sections, the CLI header  * etc. all the data is written in
 * assembly->pefile where it can be easily retrieved later in chunks.
 */
void
mono_image_create_pefile (MonoReflectionModuleBuilder *mb) {
	MonoMSDOSHeader *msdos;
	MonoDotNetHeader *header;
	MonoSectionTable *section;
	MonoCLIHeader *cli_header;
	guint32 size, image_size, virtual_base, text_offset;
	guint32 header_start, section_start, file_offset, virtual_offset;
	MonoDynamicImage *assembly;
	MonoReflectionAssemblyBuilder *assemblyb;
	MonoDynamicStream *pefile;
	int i, nsections;
	guint32 *rva, value;
	guint16 *data16;
	guchar *p;
	static const unsigned char msheader[] = {
		0x4d, 0x5a, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
		0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
		0x0e, 0x1f, 0xba, 0x0e, 0x00, 0xb4, 0x09, 0xcd,  0x21, 0xb8, 0x01, 0x4c, 0xcd, 0x21, 0x54, 0x68,
		0x69, 0x73, 0x20, 0x70, 0x72, 0x6f, 0x67, 0x72,  0x61, 0x6d, 0x20, 0x63, 0x61, 0x6e, 0x6e, 0x6f,
		0x74, 0x20, 0x62, 0x65, 0x20, 0x72, 0x75, 0x6e,  0x20, 0x69, 0x6e, 0x20, 0x44, 0x4f, 0x53, 0x20,
		0x6d, 0x6f, 0x64, 0x65, 0x2e, 0x0d, 0x0d, 0x0a,  0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	assemblyb = mb->assemblyb;

	mono_image_basic_init (assemblyb);
	assembly = mb->dynamic_image;

	/* already created */
	if (assembly->pefile.index)
		return;
	
	mono_image_build_metadata (mb);

	if (assemblyb->resources) {
		int len = mono_array_length (assemblyb->resources);
		for (i = 0; i < len; ++i)
			assembly_add_resource (assembly, (MonoReflectionResource*)mono_array_addr (assemblyb->resources, MonoReflectionResource, i));
	}

	
	build_compressed_metadata (assembly);

	nsections = calc_section_size (assembly);

	pefile = &assembly->pefile;

	/* The DOS header and stub */
	g_assert (sizeof (MonoMSDOSHeader) == sizeof (msheader));
	mono_image_add_stream_data (pefile, msheader, sizeof (msheader));

	/* the dotnet header */
	header_start = mono_image_add_stream_zero (pefile, sizeof (MonoDotNetHeader));

	/* the section tables */
	section_start = mono_image_add_stream_zero (pefile, sizeof (MonoSectionTable) * nsections);

	file_offset = section_start + sizeof (MonoSectionTable) * nsections;
	virtual_offset = VIRT_ALIGN;
	image_size = 0;

	for (i = 0; i < MONO_SECTION_MAX; ++i) {
		if (!assembly->sections [i].size)
			continue;
		/* align offsets */
		file_offset += FILE_ALIGN - 1;
		file_offset &= ~(FILE_ALIGN - 1);
		virtual_offset += VIRT_ALIGN - 1;
		virtual_offset &= ~(VIRT_ALIGN - 1);

		assembly->sections [i].offset = file_offset;
		assembly->sections [i].rva = virtual_offset;

		file_offset += assembly->sections [i].size;
		virtual_offset += assembly->sections [i].size;
		image_size += (assembly->sections [i].size + VIRT_ALIGN - 1) & ~(VIRT_ALIGN - 1);
	}

	file_offset += FILE_ALIGN - 1;
	file_offset &= ~(FILE_ALIGN - 1);
	mono_image_add_stream_zero (pefile, file_offset - pefile->index);

	image_size += section_start + sizeof (MonoSectionTable) * nsections;

	/* back-patch info */
	msdos = (MonoMSDOSHeader*)pefile->data;
	msdos->nlast_page = GUINT16_FROM_LE (file_offset & (512 - 1));
	msdos->npages = GUINT16_FROM_LE ((file_offset + (512 - 1)) / 512);
	msdos->pe_offset = GUINT32_FROM_LE (sizeof (MonoMSDOSHeader));

	header = (MonoDotNetHeader*)(pefile->data + header_start);
	header->pesig [0] = 'P';
	header->pesig [1] = 'E';
	
	header->coff.coff_machine = GUINT16_FROM_LE (0x14c);
	header->coff.coff_sections = GUINT16_FROM_LE (nsections);
	header->coff.coff_time = GUINT32_FROM_LE (time (NULL));
	header->coff.coff_opt_header_size = GUINT16_FROM_LE (sizeof (MonoDotNetHeader) - sizeof (MonoCOFFHeader) - 4);
	if (assemblyb->pekind == 1) {
		/* it's a dll */
		header->coff.coff_attributes = GUINT16_FROM_LE (0x210e);
	} else {
		/* it's an exe */
		header->coff.coff_attributes = GUINT16_FROM_LE (0x010e);
	}

	virtual_base = 0x400000; /* FIXME: 0x10000000 if a DLL */

	header->pe.pe_magic = GUINT16_FROM_LE (0x10B);
	header->pe.pe_major = 6;
	header->pe.pe_minor = 0;
	size = assembly->sections [MONO_SECTION_TEXT].size;
	size += FILE_ALIGN - 1;
	size &= ~(FILE_ALIGN - 1);
	header->pe.pe_code_size = GUINT32_FROM_LE(size);
	size = assembly->sections [MONO_SECTION_RSRC].size;
	size += FILE_ALIGN - 1;
	size &= ~(FILE_ALIGN - 1);
	header->pe.pe_data_size = GUINT32_FROM_LE(size);
	g_assert (START_TEXT_RVA == assembly->sections [MONO_SECTION_TEXT].rva);
	header->pe.pe_rva_code_base = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_TEXT].rva);
	header->pe.pe_rva_data_base = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_RSRC].rva);
	/* pe_rva_entry_point always at the beginning of the text section */
	header->pe.pe_rva_entry_point = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_TEXT].rva);

	header->nt.pe_image_base = GUINT32_FROM_LE (virtual_base);
	header->nt.pe_section_align = GUINT32_FROM_LE (VIRT_ALIGN);
	header->nt.pe_file_alignment = GUINT32_FROM_LE (FILE_ALIGN);
	header->nt.pe_os_major = GUINT16_FROM_LE (4);
	header->nt.pe_os_minor = GUINT16_FROM_LE (0);
	header->nt.pe_subsys_major = GUINT16_FROM_LE (4);
	size = section_start;
	size += FILE_ALIGN - 1;
	size &= ~(FILE_ALIGN - 1);
	header->nt.pe_header_size = GUINT32_FROM_LE (size);
	size = image_size;
	size += VIRT_ALIGN - 1;
	size &= ~(VIRT_ALIGN - 1);
	header->nt.pe_image_size = GUINT32_FROM_LE (size);

	//
	// Translate the PEFileKind value to the value expected by the Windows loader
	//
	{
		short kind = assemblyb->pekind;

		//
		// PEFileKinds.ConsoleApplication == 2
		// PEFileKinds.WindowApplication == 3
		//
		// need to get:
		//     IMAGE_SUBSYSTEM_WINDOWS_GUI 2 // Image runs in the Windows GUI subsystem.
                //     IMAGE_SUBSYSTEM_WINDOWS_CUI 3 // Image runs in the Windows character subsystem.
		if (kind == 2)
			kind = 3;
		else if (kind == 3)
			kind = 2;
		
		header->nt.pe_subsys_required = GUINT16_FROM_LE (kind);
	}    
	header->nt.pe_stack_reserve = GUINT32_FROM_LE (0x00100000);
	header->nt.pe_stack_commit = GUINT32_FROM_LE (0x00001000);
	header->nt.pe_heap_reserve = GUINT32_FROM_LE (0x00100000);
	header->nt.pe_heap_commit = GUINT32_FROM_LE (0x00001000);
	header->nt.pe_loader_flags = GUINT32_FROM_LE (0);
	header->nt.pe_data_dir_count = GUINT32_FROM_LE (16);

	/* fill data directory entries */

	header->datadir.pe_resource_table.size = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_RSRC].size);
	header->datadir.pe_resource_table.rva = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_RSRC].rva);

	header->datadir.pe_reloc_table.size = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_RELOC].size);
	header->datadir.pe_reloc_table.rva = GUINT32_FROM_LE (assembly->sections [MONO_SECTION_RELOC].rva);

	header->datadir.pe_cli_header.size = GUINT32_FROM_LE (72);
	header->datadir.pe_cli_header.rva = GUINT32_FROM_LE (assembly->text_rva + assembly->cli_header_offset);
	header->datadir.pe_iat.size = GUINT32_FROM_LE (8);
	header->datadir.pe_iat.rva = GUINT32_FROM_LE (assembly->text_rva + assembly->iat_offset);
	/* patch imported function RVA name */
	rva = (guint32*)(assembly->code.data + assembly->iat_offset);
	*rva = GUINT32_FROM_LE (assembly->text_rva + assembly->imp_names_offset);

	/* the import table */
	header->datadir.pe_import_table.size = GUINT32_FROM_LE (79); /* FIXME: magic number? */
	header->datadir.pe_import_table.rva = GUINT32_FROM_LE (assembly->text_rva + assembly->idt_offset);
	/* patch imported dll RVA name and other entries in the dir */
	rva = (guint32*)(assembly->code.data + assembly->idt_offset + G_STRUCT_OFFSET (MonoIDT, name_rva));
	*rva = GUINT32_FROM_LE (assembly->text_rva + assembly->imp_names_offset + 12); /* 12 is strlen+1 of func name */
	rva = (guint32*)(assembly->code.data + assembly->idt_offset + G_STRUCT_OFFSET (MonoIDT, import_address_table_rva));
	*rva = GUINT32_FROM_LE (assembly->text_rva + assembly->iat_offset);
	rva = (guint32*)(assembly->code.data + assembly->idt_offset + G_STRUCT_OFFSET (MonoIDT, import_lookup_table));
	*rva = GUINT32_FROM_LE (assembly->text_rva + assembly->ilt_offset);

	p = (assembly->code.data + assembly->ilt_offset);
	value = (assembly->text_rva + assembly->imp_names_offset - 2);
	*p++ = (value) & 0xff;
	*p++ = (value >> 8) & (0xff);
	*p++ = (value >> 16) & (0xff);
	*p++ = (value >> 24) & (0xff);

	/* the CLI header info */
	cli_header = (MonoCLIHeader*)(assembly->code.data + assembly->cli_header_offset);
	cli_header->ch_size = GUINT32_FROM_LE (72);
	cli_header->ch_runtime_major = GUINT16_FROM_LE (2);
	cli_header->ch_flags = GUINT32_FROM_LE (CLI_FLAGS_ILONLY);
	if (assemblyb->entry_point) 
		cli_header->ch_entry_point = GUINT32_FROM_LE (assemblyb->entry_point->table_idx | MONO_TOKEN_METHOD_DEF);
	else
		cli_header->ch_entry_point = GUINT32_FROM_LE (0);
	/* The embedded managed resources */
	text_offset = assembly->text_rva + assembly->code.index;
	cli_header->ch_resources.rva = GUINT32_FROM_LE (text_offset);
	cli_header->ch_resources.size = GUINT32_FROM_LE (assembly->resources.index);
	text_offset += assembly->resources.index;
	cli_header->ch_metadata.rva = GUINT32_FROM_LE (text_offset);
	cli_header->ch_metadata.size = GUINT32_FROM_LE (assembly->meta_size);
	text_offset += assembly->meta_size;
	if (assembly->strong_name_size) {
		cli_header->ch_strong_name.rva = GUINT32_FROM_LE (text_offset);
		cli_header->ch_strong_name.size = GUINT32_FROM_LE (assembly->strong_name_size);
		text_offset += assembly->strong_name_size;
	}

	/* write the section tables and section content */
	section = (MonoSectionTable*)(pefile->data + section_start);
	for (i = 0; i < MONO_SECTION_MAX; ++i) {
		static const char *section_names [] = {
			".text", ".rsrc", ".reloc"
		};
		if (!assembly->sections [i].size)
			continue;
		strcpy (section->st_name, section_names [i]);
		/*g_print ("output section %s (%d), size: %d\n", section->st_name, i, assembly->sections [i].size);*/
		section->st_virtual_address = GUINT32_FROM_LE (assembly->sections [i].rva);
		section->st_virtual_size = GUINT32_FROM_LE (assembly->sections [i].size);
		section->st_raw_data_size = GUINT32_FROM_LE (GUINT32_TO_LE (section->st_virtual_size) + (FILE_ALIGN - 1));
		section->st_raw_data_size &= GUINT32_FROM_LE (~(FILE_ALIGN - 1));
		section->st_raw_data_ptr = GUINT32_FROM_LE (assembly->sections [i].offset);
		section->st_flags = GUINT32_FROM_LE (assembly->sections [i].attrs);
		switch (i) {
		case MONO_SECTION_TEXT:
			/* patch entry point */
			p = (assembly->code.data + 2);
			value = (virtual_base + assembly->text_rva + assembly->iat_offset);
			*p++ = (value) & 0xff;
			*p++ = (value >> 8) & 0xff;
			*p++ = (value >> 16) & 0xff;
			*p++ = (value >> 24) & 0xff;

			text_offset = assembly->sections [i].offset;
			memcpy (pefile->data + text_offset, assembly->code.data, assembly->code.index);
			text_offset += assembly->code.index;
			memcpy (pefile->data + text_offset, assembly->resources.data, assembly->resources.index);
			text_offset += assembly->resources.index;
			memcpy (pefile->data + text_offset, assembly->image.raw_metadata, assembly->meta_size);
			text_offset += assembly->meta_size;
			memcpy (pefile->data + text_offset, assembly->strong_name, assembly->strong_name_size);

			g_free (assembly->image.raw_metadata);
			break;
		case MONO_SECTION_RELOC:
			rva = (guint32*)(pefile->data + assembly->sections [i].offset);
			*rva = GUINT32_FROM_LE (assembly->text_rva);
			++rva;
			*rva = GUINT32_FROM_LE (12);
			++rva;
			data16 = (guint16*)rva;
			/* 
			 * the entrypoint is always at the start of the text section 
			 * 3 is IMAGE_REL_BASED_HIGHLOW
			 * 2 is patch_size_rva - text_rva
			 */
			*data16 = GUINT16_FROM_LE ((3 << 12) + (2));
			data16++;
			*data16 = 0; /* terminate */
			break;
		case MONO_SECTION_RSRC:
		default:
			g_assert_not_reached ();
		}
		section++;
	}
	
	/* check that the file is properly padded */
#if 0
	{
		FILE *f = fopen ("mypetest.exe", "w");
		fwrite (pefile->data, pefile->index, 1, f);
		fclose (f);
	}
#endif
}

/*
 * We need to return always the same object for MethodInfo, FieldInfo etc..
 * but we need to consider the reflected type.
 * type uses a different hash, since it uses custom hash/equal functions.
 */

typedef struct {
	gpointer item;
	MonoClass *refclass;
} ReflectedEntry;

static gboolean
reflected_equal (gconstpointer a, gconstpointer b) {
	const ReflectedEntry *ea = a;
	const ReflectedEntry *eb = b;

	return (ea->item == eb->item) && (ea->refclass == eb->refclass);
}

static guint
reflected_hash (gconstpointer a) {
	const ReflectedEntry *ea = a;
	return GPOINTER_TO_UINT (ea->item);
}

#define CHECK_OBJECT(t,p,k)	\
	do {	\
		t _obj;	\
		ReflectedEntry e; 	\
		e.item = (p);	\
		e.refclass = (k);	\
		mono_domain_lock (domain);	\
		if (!domain->refobject_hash)	\
			domain->refobject_hash = mono_g_hash_table_new (reflected_hash, reflected_equal);	\
		if ((_obj = mono_g_hash_table_lookup (domain->refobject_hash, &e))) {	\
			mono_domain_unlock (domain);	\
			return _obj;	\
		}	\
	} while (0)

#if HAVE_BOEHM_GC
#define ALLOC_REFENTRY GC_MALLOC (sizeof (ReflectedEntry))
#else
#define ALLOC_REFENTRY mono_mempool_alloc (domain->mp, sizeof (ReflectedEntry))
#endif

#define CACHE_OBJECT(p,o,k)	\
	do {	\
		ReflectedEntry *e = ALLOC_REFENTRY; 	\
		e->item = (p);	\
		e->refclass = (k);	\
		mono_g_hash_table_insert (domain->refobject_hash, e,o);	\
		mono_domain_unlock (domain);	\
	} while (0)

static void 
register_assembly (MonoDomain *domain, MonoReflectionAssembly *res, MonoAssembly *assembly)
{
	/* this is done only once */
	mono_domain_lock (domain);
	CACHE_OBJECT (assembly, res, NULL);
}

static void
register_module (MonoDomain *domain, MonoReflectionModuleBuilder *res, MonoDynamicImage *module)
{
	/* this is done only once */
	mono_domain_lock (domain);
	CACHE_OBJECT (module, res, NULL);
}

void
mono_image_module_basic_init (MonoReflectionModuleBuilder *moduleb)
{
	MonoDynamicImage *image = moduleb->dynamic_image;
	MonoReflectionAssemblyBuilder *ab = moduleb->assemblyb;
	if (!image) {
		/*
		 * FIXME: we already created an image in mono_image_basic_init (), but
		 * we don't know which module it belongs to, since that is only 
		 * determined at assembly save time.
		 */
		//image = (MonoDynamicImage*)ab->dynamic_assembly->assembly.image;
		image = create_dynamic_mono_image (ab->dynamic_assembly, mono_string_to_utf8 (ab->name), mono_string_to_utf8 (moduleb->module.fqname));

		moduleb->module.image = &image->image;
		moduleb->dynamic_image = image;
		register_module (mono_object_domain (moduleb), moduleb, image);
	}
}

/*
 * mono_assembly_get_object:
 * @domain: an app domain
 * @assembly: an assembly
 *
 * Return an System.Reflection.Assembly object representing the MonoAssembly @assembly.
 */
MonoReflectionAssembly*
mono_assembly_get_object (MonoDomain *domain, MonoAssembly *assembly)
{
	static MonoClass *System_Reflection_Assembly;
	MonoReflectionAssembly *res;
	
	CHECK_OBJECT (MonoReflectionAssembly *, assembly, NULL);
	if (!System_Reflection_Assembly)
		System_Reflection_Assembly = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "Assembly");
	res = (MonoReflectionAssembly *)mono_object_new (domain, System_Reflection_Assembly);
	res->assembly = assembly;
	CACHE_OBJECT (assembly, res, NULL);
	return res;
}



MonoReflectionModule*   
mono_module_get_object   (MonoDomain *domain, MonoImage *image)
{
	static MonoClass *System_Reflection_Module;
	MonoReflectionModule *res;
	
	CHECK_OBJECT (MonoReflectionModule *, image, NULL);
	if (!System_Reflection_Module)
		System_Reflection_Module = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "Module");
	res = (MonoReflectionModule *)mono_object_new (domain, System_Reflection_Module);

	res->image = image;
	res->assembly = (MonoReflectionAssembly *) mono_assembly_get_object(domain, image->assembly);

	res->fqname    = mono_string_new (domain, image->name);
	res->name      = mono_string_new (domain, image->name);
	res->scopename = mono_string_new (domain, image->module_name);

	CACHE_OBJECT (image, res, NULL);
	return res;
}

MonoReflectionModule*   
mono_module_file_get_object (MonoDomain *domain, MonoImage *image, int table_index)
{
	static MonoClass *System_Reflection_Module;
	MonoReflectionModule *res;
	MonoTableInfo *table;
	guint32 cols [MONO_FILE_SIZE];
	const char *name;
	guint32 i, name_idx;
	const char *val;
	
	if (!System_Reflection_Module)
		System_Reflection_Module = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "Module");
	res = (MonoReflectionModule *)mono_object_new (domain, System_Reflection_Module);

	table = &image->tables [MONO_TABLE_FILE];
	g_assert (table_index < table->rows);
	mono_metadata_decode_row (table, table_index, cols, MONO_FILE_SIZE);

	res->image = 0;
	res->assembly = (MonoReflectionAssembly *) mono_assembly_get_object(domain, image->assembly);
	name = mono_metadata_string_heap (image, cols [MONO_FILE_NAME]);

	// Check whenever the row has a corresponding row in the moduleref table
	table = &image->tables [MONO_TABLE_MODULEREF];
	for (i = 0; i < table->rows; ++i) {
		name_idx = mono_metadata_decode_row_col (table, i, MONO_MODULEREF_NAME);
		val = mono_metadata_string_heap (image, name_idx);
		if (strcmp (val, name) == 0)
			res->image = image->modules [i];
	}

	res->fqname    = mono_string_new (domain, name);
	res->name      = mono_string_new (domain, name);
	res->scopename = mono_string_new (domain, name);
	res->is_resource = cols [MONO_FILE_FLAGS] && FILE_CONTAINS_NO_METADATA;

	return res;
}

static gboolean
mymono_metadata_type_equal (MonoType *t1, MonoType *t2)
{
	if ((t1->type != t2->type) ||
	    (t1->byref != t2->byref))
		return FALSE;

	switch (t1->type) {
	case MONO_TYPE_VOID:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
	case MONO_TYPE_STRING:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_TYPEDBYREF:
		return TRUE;
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
		return t1->data.klass == t2->data.klass;
	case MONO_TYPE_PTR:
		return mymono_metadata_type_equal (t1->data.type, t2->data.type);
	case MONO_TYPE_ARRAY:
		if (t1->data.array->rank != t2->data.array->rank)
			return FALSE;
		return t1->data.array->eklass == t2->data.array->eklass;
	case MONO_TYPE_GENERICINST: {
		int i;
		if (t1->data.generic_inst->type_argc != t2->data.generic_inst->type_argc)
			return FALSE;
		if (!mono_metadata_type_equal (t1->data.generic_inst->generic_type, t2->data.generic_inst->generic_type))
			return FALSE;
		for (i = 0; i < t1->data.generic_inst->type_argc; ++i) {
			if (!mono_metadata_type_equal (t1->data.generic_inst->type_argv [i], t2->data.generic_inst->type_argv [i]))
				return FALSE;
		}
		return TRUE;
	}
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return t1->data.generic_param == t2->data.generic_param;
	default:
		g_error ("implement type compare for %0x!", t1->type);
		return FALSE;
	}

	return FALSE;
}

static guint
mymono_metadata_type_hash (MonoType *t1)
{
	guint hash;

	hash = t1->type;

	hash |= t1->byref << 6; /* do not collide with t1->type values */
	switch (t1->type) {
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
		/* check if the distribution is good enough */
		return ((hash << 5) - hash) ^ g_str_hash (t1->data.klass->name);
	case MONO_TYPE_PTR:
		return ((hash << 5) - hash) ^ mymono_metadata_type_hash (t1->data.type);
	}
	return hash;
}

/*
 * mono_type_get_object:
 * @domain: an app domain
 * @type: a type
 *
 * Return an System.MonoType object representing the type @type.
 */
MonoReflectionType*
mono_type_get_object (MonoDomain *domain, MonoType *type)
{
	MonoReflectionType *res;
	MonoClass *klass = mono_class_from_mono_type (type);

	mono_domain_lock (domain);
	if (!domain->type_hash)
		domain->type_hash = mono_g_hash_table_new ((GHashFunc)mymono_metadata_type_hash, 
				(GCompareFunc)mymono_metadata_type_equal);
	if ((res = mono_g_hash_table_lookup (domain->type_hash, type))) {
		mono_domain_unlock (domain);
		return res;
	}
	if (klass->reflection_info && !klass->wastypebuilder) {
		//g_assert_not_reached ();
		/* should this be considered an error condition? */
		if (!type->byref) {
			mono_domain_unlock (domain);
			return klass->reflection_info;
		}
	}
	mono_class_init (klass);
	res = (MonoReflectionType *)mono_object_new (domain, mono_defaults.monotype_class);
	res->type = type;
	mono_g_hash_table_insert (domain->type_hash, type, res);
	mono_domain_unlock (domain);
	return res;
}

/*
 * mono_method_get_object:
 * @domain: an app domain
 * @method: a method
 * @refclass: the reflected type (can be NULL)
 *
 * Return an System.Reflection.MonoMethod object representing the method @method.
 */
MonoReflectionMethod*
mono_method_get_object (MonoDomain *domain, MonoMethod *method, MonoClass *refclass)
{
	/*
	 * We use the same C representation for methods and constructors, but the type 
	 * name in C# is different.
	 */
	const char *cname;
	MonoClass *klass;
	MonoReflectionMethod *ret;

	if (!refclass)
		refclass = method->klass;

	CHECK_OBJECT (MonoReflectionMethod *, method, refclass);
	if (*method->name == '.' && (strcmp (method->name, ".ctor") == 0 || strcmp (method->name, ".cctor") == 0))
		cname = "MonoCMethod";
	else
		cname = "MonoMethod";
	klass = mono_class_from_name (mono_defaults.corlib, "System.Reflection", cname);

	ret = (MonoReflectionMethod*)mono_object_new (domain, klass);
	ret->method = method;
	ret->name = mono_string_new (domain, method->name);
	ret->reftype = mono_type_get_object (domain, &refclass->byval_arg);
	CACHE_OBJECT (method, ret, refclass);
	return ret;
}

/*
 * mono_field_get_object:
 * @domain: an app domain
 * @klass: a type
 * @field: a field
 *
 * Return an System.Reflection.MonoField object representing the field @field
 * in class @klass.
 */
MonoReflectionField*
mono_field_get_object (MonoDomain *domain, MonoClass *klass, MonoClassField *field)
{
	MonoReflectionField *res;
	MonoClass *oklass;

	CHECK_OBJECT (MonoReflectionField *, field, klass);
	oklass = mono_class_from_name (mono_defaults.corlib, "System.Reflection", "MonoField");
	res = (MonoReflectionField *)mono_object_new (domain, oklass);
	res->klass = klass;
	res->field = field;
	res->name = mono_string_new (domain, field->name);
	res->attrs = field->type->attrs;
	res->type = mono_type_get_object (domain, field->type);
	CACHE_OBJECT (field, res, klass);
	return res;
}

/*
 * mono_property_get_object:
 * @domain: an app domain
 * @klass: a type
 * @property: a property
 *
 * Return an System.Reflection.MonoProperty object representing the property @property
 * in class @klass.
 */
MonoReflectionProperty*
mono_property_get_object (MonoDomain *domain, MonoClass *klass, MonoProperty *property)
{
	MonoReflectionProperty *res;
	MonoClass *oklass;

	CHECK_OBJECT (MonoReflectionProperty *, property, klass);
	oklass = mono_class_from_name (mono_defaults.corlib, "System.Reflection", "MonoProperty");
	res = (MonoReflectionProperty *)mono_object_new (domain, oklass);
	res->klass = klass;
	res->property = property;
	CACHE_OBJECT (property, res, klass);
	return res;
}

/*
 * mono_event_get_object:
 * @domain: an app domain
 * @klass: a type
 * @event: a event
 *
 * Return an System.Reflection.MonoEvent object representing the event @event
 * in class @klass.
 */
MonoReflectionEvent*
mono_event_get_object (MonoDomain *domain, MonoClass *klass, MonoEvent *event)
{
	MonoReflectionEvent *res;
	MonoClass *oklass;

	CHECK_OBJECT (MonoReflectionEvent *, event, klass);
	oklass = mono_class_from_name (mono_defaults.corlib, "System.Reflection", "MonoEvent");
	res = (MonoReflectionEvent *)mono_object_new (domain, oklass);
	res->klass = klass;
	res->event = event;
	CACHE_OBJECT (event, res, klass);
	return res;
}

/*
 * mono_param_get_objects:
 * @domain: an app domain
 * @method: a method
 *
 * Return an System.Reflection.ParameterInfo array object representing the parameters
 * in the method @method.
 */
MonoArray*
mono_param_get_objects (MonoDomain *domain, MonoMethod *method)
{
	static MonoClass *System_Reflection_ParameterInfo;
	MonoArray *res = NULL;
	MonoReflectionMethod *member = NULL;
	MonoReflectionParameter *param = NULL;
	char **names;
	int i;

	if (!System_Reflection_ParameterInfo)
		System_Reflection_ParameterInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "ParameterInfo");
	
	if (!method->signature->param_count)
		return mono_array_new (domain, System_Reflection_ParameterInfo, 0);

	/* Note: the cache is based on the address of the signature into the method
	 * since we already cache MethodInfos with the method as keys.
	 */
	CHECK_OBJECT (MonoArray*, &(method->signature), NULL);

	member = mono_method_get_object (domain, method, NULL);
	names = g_new (char *, method->signature->param_count);
	mono_method_get_param_names (method, (const char **) names);

	res = mono_array_new (domain, System_Reflection_ParameterInfo, method->signature->param_count);
	for (i = 0; i < method->signature->param_count; ++i) {
		param = (MonoReflectionParameter *)mono_object_new (domain, 
															System_Reflection_ParameterInfo);
		param->ClassImpl = mono_type_get_object (domain, method->signature->params [i]);
		param->DefaultValueImpl = NULL; /* FIXME */
		param->MemberImpl = (MonoObject*)member;
		param->NameImpl = mono_string_new (domain, names [i]);
		param->PositionImpl = i;
		param->AttrsImpl = method->signature->params [i]->attrs;
		mono_array_set (res, gpointer, i, param);
	}
	g_free (names);
	CACHE_OBJECT (&(method->signature), res, NULL);
	return res;
}

static int
assembly_name_to_aname (MonoAssemblyName *assembly, char *p) {
	int found_sep;
	char *s;

	memset (assembly, 0, sizeof (MonoAssemblyName));
	assembly->name = p;
	assembly->culture = "";
	
	while (*p && (isalnum (*p) || *p == '.' || *p == '-'))
		p++;
	found_sep = 0;
	while (*p == ' ' || *p == ',') {
		*p++ = 0;
		found_sep = 1;
		continue;
	}
	/* failed */
	if (!found_sep)
		return 1;
	while (*p) {
		if (*p == 'V' && strncmp (p, "Version=", 8) == 0) {
			p += 8;
			assembly->major = strtoul (p, &s, 10);
			if (s == p || *s != '.')
				return 1;
			p = ++s;
			assembly->minor = strtoul (p, &s, 10);
			if (s == p || *s != '.')
				return 1;
			p = ++s;
			assembly->build = strtoul (p, &s, 10);
			if (s == p || *s != '.')
				return 1;
			p = ++s;
			assembly->revision = strtoul (p, &s, 10);
			if (s == p)
				return 1;
		} else if (*p == 'C' && strncmp (p, "Culture=", 8) == 0) {
			p += 8;
			if (strncmp (p, "neutral", 7) == 0) {
				assembly->culture = "";
				p += 7;
			} else {
				assembly->culture = p;
				while (*p && *p != ',') {
					p++;
				}
			}
		} else if (*p == 'P' && strncmp (p, "PublicKeyToken=", 15) == 0) {
			p += 15;
			s = p;
			while (*s && isxdigit (*s)) {
				*s = tolower (*s);
				s++;
			}
			assembly->hash_len = s - p;
			if (!(s-p) || ((s-p) & 1))
				return 1;
			assembly->hash_value = s = p;
			while (*s && isxdigit (*s)) {
				int val;
				val = *s >= '0' && *s <= '9'? *s - '0': *s - 'a' + 10;
				s++;
				*p = val << 4;
				*p |= *s >= '0' && *s <= '9'? *s - '0': *s - 'a' + 10;
				p++;
			}
			p = s;
		} else {
			while (*p && *p != ',')
				p++;
		}
		found_sep = 0;
		while (*p == ' ' || *p == ',') {
			*p++ = 0;
			found_sep = 1;
			continue;
		}
		/* failed */
		if (!found_sep)
			return 1;
	}

	return 0;
}

/*
 * mono_reflection_parse_type:
 * @name: type name
 *
 * Parse a type name as accepted by the GetType () method and output the info
 * extracted in the info structure.
 * the name param will be mangled, so, make a copy before passing it to this function.
 * The fields in info will be valid until the memory pointed to by name is valid.
 * Returns 0 on parse error.
 * See also mono_type_get_name () below.
 */
int
mono_reflection_parse_type (char *name, MonoTypeNameParse *info) {

	char *start, *p, *w, *last_point, *startn;
	int in_modifiers = 0;
	int isbyref = 0, rank;

	start = p = w = name;

	memset (&info->assembly, 0, sizeof (MonoAssemblyName));
	info->name = info->name_space = NULL;
	info->nested = NULL;
	info->modifiers = NULL;

	/* last_point separates the namespace from the name */
	last_point = NULL;

	while (*p) {
		switch (*p) {
		case '+':
			*p = 0; /* NULL terminate the name */
			startn = p + 1;
			info->nested = g_list_append (info->nested, startn);
			/* we have parsed the nesting namespace + name */
			if (info->name)
				break;
			if (last_point) {
				info->name_space = start;
				*last_point = 0;
				info->name = last_point + 1;
			} else {
				info->name_space = (char *)"";
				info->name = start;
			}
			break;
		case '.':
			last_point = w;
			break;
		case '\\':
			++p;
			break;
		case '&':
		case '*':
		case '[':
		case ',':
			in_modifiers = 1;
			break;
		default:
			break;
		}
		if (in_modifiers)
			break;
		*w++ = *p++;
	}
	
	if (!info->name) {
		if (last_point) {
			info->name_space = start;
			*last_point = 0;
			info->name = last_point + 1;
		} else {
			info->name_space = (char *)"";
			info->name = start;
		}
	}
	while (*p) {
		switch (*p) {
		case '&':
			if (isbyref) /* only one level allowed by the spec */
				return 0;
			isbyref = 1;
			info->modifiers = g_list_append (info->modifiers, GUINT_TO_POINTER (0));
			*p++ = 0;
			break;
		case '*':
			info->modifiers = g_list_append (info->modifiers, GUINT_TO_POINTER (-1));
			*p++ = 0;
			break;
		case '[':
			rank = 1;
			*p++ = 0;
			while (*p) {
				if (*p == ']')
					break;
				if (*p == ',')
					rank++;
				else if (*p != '*') /* '*' means unknown lower bound */
					return 0;
				++p;
			}
			if (*p++ != ']')
				return 0;
			info->modifiers = g_list_append (info->modifiers, GUINT_TO_POINTER (rank));
			break;
		case ',':
			*p++ = 0;
			while (*p) {
				if (*p == ' ') {
					++p;
					continue;
				}
				break;
			}
			if (!*p)
				return 0; /* missing assembly name */
			if (!assembly_name_to_aname (&info->assembly, p))
				return 0;
			break;
		default:
			return 0;
			break;
		}
		if (info->assembly.name)
			break;
	}
	*w = 0; /* terminate class name */
	if (!info->name || !*info->name)
		return 0;
	/* add other consistency checks */
	return 1;
}

static MonoType*
mono_reflection_get_type_internal (MonoImage* image, MonoTypeNameParse *info, gboolean ignorecase)
{
	MonoClass *klass;
	GList *mod;
	int modval;
	
	if (!image)
		image = mono_defaults.corlib;

	if (ignorecase)
		klass = mono_class_from_name_case (image, info->name_space, info->name);
	else
		klass = mono_class_from_name (image, info->name_space, info->name);
	if (!klass)
		return NULL;
	for (mod = info->nested; mod; mod = mod->next) {
		GList *nested;

		mono_class_init (klass);
		nested = klass->nested_classes;
		klass = NULL;
		while (nested) {
			klass = nested->data;
			if (ignorecase) {
				if (g_strcasecmp (klass->name, mod->data) == 0)
					break;
			} else {
				if (strcmp (klass->name, mod->data) == 0)
					break;
			}
			klass = NULL;
			nested = nested->next;
		}
		if (!klass)
			break;
	}
	if (!klass)
		return NULL;
	mono_class_init (klass);
	for (mod = info->modifiers; mod; mod = mod->next) {
		modval = GPOINTER_TO_UINT (mod->data);
		if (!modval) { /* byref: must be last modifier */
			return &klass->this_arg;
		} else if (modval == -1) {
			klass = mono_ptr_class_get (&klass->byval_arg);
		} else { /* array rank */
			klass = mono_array_class_get (klass, modval);
		}
		mono_class_init (klass);
	}

	return &klass->byval_arg;
}

/*
 * mono_reflection_get_type:
 * @image: a metadata context
 * @info: type description structure
 * @ignorecase: flag for case-insensitive string compares
 *
 * Build a MonoType from the type description in @info.
 * 
 */

MonoType*
mono_reflection_get_type (MonoImage* image, MonoTypeNameParse *info, gboolean ignorecase)
{
	MonoType *type;
	MonoReflectionAssembly *assembly;
	GString *fullName;
	GList *mod;

	type = mono_reflection_get_type_internal (image, info, ignorecase);
	if (type)
		return type;
	if (!mono_domain_has_type_resolve (mono_domain_get ()))
		return NULL;
	
	// Reconstruct the type name
	fullName = g_string_new ("");
	if (info->name_space && (info->name_space [0] != '\0'))
		g_string_printf (fullName, "%s.%s", info->name_space, info->name);
	else
		g_string_printf (fullName, info->name);
	for (mod = info->nested; mod; mod = mod->next)
		g_string_append_printf (fullName, "+%s", (char*)mod->data);

	assembly = 
		mono_domain_try_type_resolve (
			mono_domain_get (), fullName->str, NULL);
	if (assembly && (!image || (assembly->assembly->image == image)))
		type = mono_reflection_get_type_internal (assembly->assembly->image, 
										 info, ignorecase);
	g_string_free (fullName, TRUE);
	return type;
}

/*
 * mono_reflection_type_from_name:
 * @name: type name.
 * @image: a metadata context (can be NULL).
 *
 * Retrieves a MonoType from its @name. If the name is not fully qualified,
 * it defaults to get the type from @image or, if @image is NULL or loading
 * from it fails, uses corlib.
 * 
 */
MonoType*
mono_reflection_type_from_name (char *name, MonoImage *image)
{
	MonoType *type;
	MonoTypeNameParse info;
	
	/*g_print ("requested type %s\n", str);*/
	if (!mono_reflection_parse_type (name, &info)) {
		g_list_free (info.modifiers);
		g_list_free (info.nested);
		return NULL;
	}

	if (info.assembly.name) {
		image = mono_image_loaded (info.assembly.name);
		/* do we need to load if it's not already loaded? */
		if (!image) {
			g_list_free (info.modifiers);
			g_list_free (info.nested);
			return NULL;
		}
	} else if (image == NULL) {
		image = mono_defaults.corlib;
	}

	type = mono_reflection_get_type (image, &info, FALSE);
	if (type == NULL && !info.assembly.name && image != mono_defaults.corlib) {
		image = mono_defaults.corlib;
		type = mono_reflection_get_type (image, &info, FALSE);
	}
	
	g_list_free (info.modifiers);
	g_list_free (info.nested);
	return type;
}

static void*
load_cattr_value (MonoImage *image, MonoType *t, const char *p, const char **end)
{
	int slen, type = t->type;
handle_enum:
	switch (type) {
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
	case MONO_TYPE_BOOLEAN: {
		MonoBoolean *bval = g_malloc (sizeof (MonoBoolean));
		*bval = *p;
		*end = p + 1;
		return bval;
	}
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2: {
		guint16 *val = g_malloc (sizeof (guint16));
		*val = read16 (p);
		*end = p + 2;
		return val;
	}
#if SIZEOF_VOID_P == 4
	case MONO_TYPE_U:
	case MONO_TYPE_I:
#endif
	case MONO_TYPE_R4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I4: {
		guint32 *val = g_malloc (sizeof (guint32));
		*val = read32 (p);
		*end = p + 4;
		return val;
	}
#if SIZEOF_VOID_P == 8
	case MONO_TYPE_U: /* error out instead? this should probably not happen */
	case MONO_TYPE_I:
#endif
	case MONO_TYPE_R8:
	case MONO_TYPE_U8:
	case MONO_TYPE_I8: {
		guint64 *val = g_malloc (sizeof (guint64));
		*val = read64 (p);
		*end = p + 8;
		return val;
	}
	case MONO_TYPE_VALUETYPE:
		if (t->data.klass->enumtype) {
			type = t->data.klass->enum_basetype->type;
			goto handle_enum;
		} else {
			g_error ("generic valutype %s not handled in custom attr value decoding", t->data.klass->name);
		}
		break;
	case MONO_TYPE_STRING:
		if (*p == (char)0xFF) {
			*end = p + 1;
			return NULL;
		}
		slen = mono_metadata_decode_value (p, &p);
		*end = p + slen;
		return mono_string_new_len (mono_domain_get (), p, slen);
	case MONO_TYPE_CLASS: {
		char *n;
		MonoType *t;
		if (*p == (char)0xFF) {
			*end = p + 1;
			return NULL;
		}
handle_type:
		slen = mono_metadata_decode_value (p, &p);
		n = g_memdup (p, slen + 1);
		n [slen] = 0;
		t = mono_reflection_type_from_name (n, image);
		if (!t)
			g_warning ("Cannot load type '%s'", n);
		g_free (n);
		*end = p + slen;
		return mono_type_get_object (mono_domain_get (), t);
	}
	case MONO_TYPE_OBJECT: {
		char subt = *p++;
		MonoObject *obj;
		MonoClass *subc;
		void *val;

		if (subt == 0x50) {
			goto handle_type;
		} else if (subt == 0x0E) {
			type = MONO_TYPE_STRING;
			goto handle_enum;
		} else if (subt == 0x55) {
			char *n;
			MonoType *t;
			slen = mono_metadata_decode_value (p, &p);
			n = g_memdup (p, slen + 1);
			n [slen] = 0;
			t = mono_reflection_type_from_name (n, image);
			if (!t)
				g_warning ("Cannot load type '%s'", n);
			g_free (n);
			p += slen;
			subc = mono_class_from_mono_type (t);
		} else if (subt >= MONO_TYPE_BOOLEAN && subt <= MONO_TYPE_R8) {
			MonoType simple_type = {{0}};
			simple_type.type = subt;
			subc = mono_class_from_mono_type (&simple_type);
		} else {
			g_error ("Unknown type 0x%02x for object type encoding in custom attr", subt);
		}
		val = load_cattr_value (image, &subc->byval_arg, p, end);
		obj = mono_object_new (mono_domain_get (), subc);
		memcpy ((char*)obj + sizeof (MonoObject), val, mono_class_value_size (subc, NULL));
		g_free (val);
		return obj;
	}
	case MONO_TYPE_SZARRAY:
	{
		MonoArray *arr;
		guint32 i, alen, basetype;
		alen = read32 (p);
		p += 4;
		if (alen == 0xffffffff) {
			*end = p;
			return NULL;
		}
		arr = mono_array_new (mono_domain_get(), t->data.klass, alen);
		basetype = t->data.klass->byval_arg.type;
		switch (basetype)
		{
			case MONO_TYPE_U1:
			case MONO_TYPE_I1:
			case MONO_TYPE_BOOLEAN:
				for (i=0;i<alen;i++)
				{
					MonoBoolean val=*p++;
					mono_array_set(arr,MonoBoolean,i,val);
				}
				break;
			case MONO_TYPE_CHAR:
			case MONO_TYPE_U2:
			case MONO_TYPE_I2:
				for (i=0;i<alen;i++)
				{
					guint16 val=read16(p);
					mono_array_set(arr,guint16,i,val);
					p+=2;
				}
				break;
			case MONO_TYPE_R4:
			case MONO_TYPE_U4:
			case MONO_TYPE_I4:
				for (i=0;i<alen;i++)
				{
					guint32 val=read32(p);
					mono_array_set(arr,guint32,i,val);
					p+=4;
				}
				break;
			case MONO_TYPE_R8:
			case MONO_TYPE_U8:
			case MONO_TYPE_I8:
				for (i=0;i<alen;i++)
				{
					guint64 val=read64(p);
					mono_array_set(arr,guint64,i,val);
					p+=8;
				}
				break;
			case MONO_TYPE_CLASS:
			case MONO_TYPE_STRING:
				for (i = 0; i < alen; i++) {
					MonoObject *item = load_cattr_value (image, &t->data.klass->byval_arg, p, &p);
					mono_array_set (arr, gpointer, i, item);
				}
				break;
			default:
				g_error("Type 0x%02x not handled in custom attr array decoding",t->data.type->type);
		}
		*end=p;
		return arr;
	}
	default:
		g_error ("Type 0x%02x not handled in custom attr value decoding", type);
	}
	return NULL;
}

static gboolean
type_is_reference (MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U:
	case MONO_TYPE_I:
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
	case MONO_TYPE_R8:
	case MONO_TYPE_R4:
	case MONO_TYPE_VALUETYPE:
		return FALSE;
	default:
		return TRUE;
	}
}

static void
free_param_data (MonoMethodSignature *sig, void **params) {
	int i;
	for (i = 0; i < sig->param_count; ++i) {
		if (!type_is_reference (sig->params [i]))
			g_free (params [i]);
	}
}

/*
 * Find the method index in the metadata methodDef table.
 * Later put these three helper methods in metadata and export them.
 */
static guint32
find_method_index (MonoMethod *method) {
	MonoClass *klass = method->klass;
	int i;

	for (i = 0; i < klass->method.count; ++i) {
		if (method == klass->methods [i])
			return klass->method.first + 1 + i;
	}
	return 0;
}

/*
 * Find the field index in the metadata FieldDef table.
 */
static guint32
find_field_index (MonoClass *klass, MonoClassField *field) {
	int i;

	for (i = 0; i < klass->field.count; ++i) {
		if (field == &klass->fields [i])
			return klass->field.first + 1 + i;
	}
	return 0;
}

/*
 * Find the property index in the metadata Property table.
 */
static guint32
find_property_index (MonoClass *klass, MonoProperty *property) {
	int i;

	for (i = 0; i < klass->property.count; ++i) {
		if (property == &klass->properties [i])
			return klass->property.first + 1 + i;
	}
	return 0;
}

/*
 * Find the event index in the metadata Event table.
 */
static guint32
find_event_index (MonoClass *klass, MonoEvent *event) {
	int i;

	for (i = 0; i < klass->event.count; ++i) {
		if (event == &klass->events [i])
			return klass->event.first + 1 + i;
	}
	return 0;
}

static MonoObject*
create_custom_attr (MonoImage *image, MonoMethod *method, 
					const char *data, guint32 len)
{
	const char *p = data;
	const char *named;
	guint32 i, j, num_named;
	MonoObject *attr;
	void **params;

	if (len < 2 || read16 (p) != 0x0001) /* Prolog */
		return NULL;

	mono_class_init (method->klass);
	/*g_print ("got attr %s\n", method->klass->name);*/
	
	params = g_new (void*, method->signature->param_count);

	/* skip prolog */
	p += 2;
	for (i = 0; i < method->signature->param_count; ++i) {
		params [i] = load_cattr_value (image, method->signature->params [i], p, &p);
	}

	named = p;
	attr = mono_object_new (mono_domain_get (), method->klass);
	mono_runtime_invoke (method, attr, params, NULL);
	free_param_data (method->signature, params);
	g_free (params);
	num_named = read16 (named);
	named += 2;
	for (j = 0; j < num_named; j++) {
		gint name_len;
		char *name, named_type, data_type;
		named_type = *named++;
		data_type = *named++; /* type of data */
		if (data_type == 0x55) {
			gint type_len;
			char *type_name;
			type_len = mono_metadata_decode_blob_size (named, &named);
			type_name = g_malloc (type_len + 1);
			memcpy (type_name, named, type_len);
			type_name [type_len] = 0;
			named += type_len;
			/* FIXME: lookup the type and check type consistency */
		}
		name_len = mono_metadata_decode_blob_size (named, &named);
		name = g_malloc (name_len + 1);
		memcpy (name, named, name_len);
		name [name_len] = 0;
		named += name_len;
		if (named_type == 0x53) {
			MonoClassField *field = mono_class_get_field_from_name (mono_object_class (attr), name);
			void *val = load_cattr_value (image, field->type, named, &named);
				mono_field_set_value (attr, field, val);
				if (!type_is_reference (field->type))
					g_free (val);
		} else if (named_type == 0x54) {
			MonoProperty *prop;
			void *pparams [1];
			MonoType *prop_type;

			prop = mono_class_get_property_from_name (mono_object_class (attr), name);
			/* can we have more that 1 arg in a custom attr named property? */
			prop_type = prop->get? prop->get->signature->ret: prop->set->signature->params [prop->set->signature->param_count - 1];
			pparams [0] = load_cattr_value (image, prop_type, named, &named);
			mono_property_set_value (prop, attr, pparams, NULL);
			if (!type_is_reference (prop_type))
				g_free (pparams [0]);
		}
		g_free (name);
	}

	return attr;
}

MonoArray*
mono_custom_attrs_construct (MonoCustomAttrInfo *cinfo)
{
	MonoArray *result;
	MonoClass *klass;
	MonoObject *attr;
	int i;

	klass = mono_class_from_name (mono_defaults.corlib, "System", "Attribute");
	result = mono_array_new (mono_domain_get (), klass, cinfo->num_attrs);
	for (i = 0; i < cinfo->num_attrs; ++i) {
		attr = create_custom_attr (cinfo->image, cinfo->attrs [i].ctor, cinfo->attrs [i].data, cinfo->attrs [i].data_size);
		mono_array_set (result, gpointer, i, attr);
	}
	return result;
}

MonoCustomAttrInfo*
mono_custom_attrs_from_index (MonoImage *image, guint32 idx)
{
	guint32 mtoken, i, len;
	guint32 cols [MONO_CUSTOM_ATTR_SIZE];
	MonoTableInfo *ca;
	MonoCustomAttrInfo *ainfo;
	GList *tmp, *list = NULL;
	const char *data;

	ca = &image->tables [MONO_TABLE_CUSTOMATTRIBUTE];

	i = mono_metadata_custom_attrs_from_index (image, idx);
	if (!i)
		return NULL;
	i --;
	while (i < ca->rows) {
		if (mono_metadata_decode_row_col (ca, i, MONO_CUSTOM_ATTR_PARENT) != idx)
			break;
		list = g_list_prepend (list, GUINT_TO_POINTER (i));
		++i;
	}
	len = g_list_length (list);
	if (!len)
		return NULL;
	ainfo = g_malloc0 (sizeof (MonoCustomAttrInfo) + sizeof (MonoCustomAttrEntry) * (len - MONO_ZERO_LEN_ARRAY));
	ainfo->num_attrs = len;
	ainfo->image = image;
	for (i = 0, tmp = list; i < len; ++i, tmp = tmp->next) {
		mono_metadata_decode_row (ca, GPOINTER_TO_UINT (tmp->data), cols, MONO_CUSTOM_ATTR_SIZE);
		mtoken = cols [MONO_CUSTOM_ATTR_TYPE] >> CUSTOM_ATTR_TYPE_BITS;
		switch (cols [MONO_CUSTOM_ATTR_TYPE] & CUSTOM_ATTR_TYPE_MASK) {
		case CUSTOM_ATTR_TYPE_METHODDEF:
			mtoken |= MONO_TOKEN_METHOD_DEF;
			break;
		case CUSTOM_ATTR_TYPE_MEMBERREF:
			mtoken |= MONO_TOKEN_MEMBER_REF;
			break;
		default:
			g_error ("Unknown table for custom attr type %08x", cols [MONO_CUSTOM_ATTR_TYPE]);
			break;
		}
		ainfo->attrs [i].ctor = mono_get_method (image, mtoken, NULL);
		if (!ainfo->attrs [i].ctor)
			g_error ("Can't find custom attr constructor image: %s mtoken: 0x%08x", image->name, mtoken);
		data = mono_metadata_blob_heap (image, cols [MONO_CUSTOM_ATTR_VALUE]);
		ainfo->attrs [i].data_size = mono_metadata_decode_value (data, &data);
		ainfo->attrs [i].data = data;
	}
	g_list_free (list);

	return ainfo;
}

MonoCustomAttrInfo*
mono_custom_attrs_from_method (MonoMethod *method)
{
	MonoCustomAttrInfo *cinfo;
	guint32 idx;
	
	if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, method)))
		return cinfo;
	idx = find_method_index (method);
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_METHODDEF;
	return mono_custom_attrs_from_index (method->klass->image, idx);
}

MonoCustomAttrInfo*
mono_custom_attrs_from_class (MonoClass *klass)
{
	MonoCustomAttrInfo *cinfo;
	guint32 idx;
	
	if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, klass)))
		return cinfo;
	idx = mono_metadata_token_index (klass->type_token);
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_TYPEDEF;
	return mono_custom_attrs_from_index (klass->image, idx);
}

MonoCustomAttrInfo*
mono_custom_attrs_from_assembly (MonoAssembly *assembly)
{
	MonoCustomAttrInfo *cinfo;
	guint32 idx;
	
	if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, assembly)))
		return cinfo;
	idx = 1; /* there is only one assembly */
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_ASSEMBLY;
	return mono_custom_attrs_from_index (assembly->image, idx);
}

MonoCustomAttrInfo*
mono_custom_attrs_from_property (MonoClass *klass, MonoProperty *property)
{
	MonoCustomAttrInfo *cinfo;
	guint32 idx;
	
	if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, property)))
		return cinfo;
	idx = find_property_index (klass, property);
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_PROPERTY;
	return mono_custom_attrs_from_index (klass->image, idx);
}

MonoCustomAttrInfo*
mono_custom_attrs_from_event (MonoClass *klass, MonoEvent *event)
{
	MonoCustomAttrInfo *cinfo;
	guint32 idx;
	
	if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, event)))
		return cinfo;
	idx = find_event_index (klass, event);
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_EVENT;
	return mono_custom_attrs_from_index (klass->image, idx);
}

MonoCustomAttrInfo*
mono_custom_attrs_from_field (MonoClass *klass, MonoClassField *field)
{
	MonoCustomAttrInfo *cinfo;
	guint32 idx;
	
	if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, field)))
		return cinfo;
	idx = find_field_index (klass, field);
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_FIELDDEF;
	return mono_custom_attrs_from_index (klass->image, idx);
}

MonoCustomAttrInfo*
mono_custom_attrs_from_param (MonoMethod *method, guint32 param)
{
	MonoTableInfo *ca;
	guint32 i, idx, method_index;
	guint32 param_list, param_last, param_pos, found;
	MonoImage *image;
	
	/* FIXME: handle dynamic custom attrs for parameters */
	/*if (dynamic_custom_attrs && (cinfo = g_hash_table_lookup (dynamic_custom_attrs, field)))
		return cinfo;*/
	image = method->klass->image;
	method_index = find_method_index (method);
	ca = &image->tables [MONO_TABLE_METHOD];

	if (method->klass->generic_inst || method->klass->gen_params ||
	    method->signature->generic_param_count) {
		// FIXME FIXME FIXME
		return NULL;
	}

	param_list = mono_metadata_decode_row_col (ca, method_index - 1, MONO_METHOD_PARAMLIST);
	if (method_index == ca->rows) {
		ca = &image->tables [MONO_TABLE_PARAM];
		param_last = ca->rows + 1;
	} else {
		param_last = mono_metadata_decode_row_col (ca, method_index, MONO_METHOD_PARAMLIST);
		ca = &image->tables [MONO_TABLE_PARAM];
	}
	found = FALSE;
	for (i = param_list; i < param_last; ++i) {
		param_pos = mono_metadata_decode_row_col (ca, i - 1, MONO_PARAM_SEQUENCE);
		if (param_pos == param) {
			found = TRUE;
			break;
		}
	}
	if (!found)
		return NULL;
	idx = i;
	idx <<= CUSTOM_ATTR_BITS;
	idx |= CUSTOM_ATTR_PARAMDEF;
	return mono_custom_attrs_from_index (image, idx);
}

/*
 * mono_reflection_get_custom_attrs:
 * @obj: a reflection object handle
 *
 * Return an array with all the custom attributes defined of the
 * reflection handle @obj. The objects are fully build.
 */
MonoArray*
mono_reflection_get_custom_attrs (MonoObject *obj)
{
	MonoClass *klass;
	MonoArray *result;
	MonoCustomAttrInfo *cinfo;
	
	MONO_ARCH_SAVE_REGS;

	klass = obj->vtable->klass;
	/* FIXME: need to handle: Module */
	if (klass == mono_defaults.monotype_class) {
		MonoReflectionType *rtype = (MonoReflectionType*)obj;
		klass = mono_class_from_mono_type (rtype->type);
		cinfo = mono_custom_attrs_from_class (klass);
	} else if (strcmp ("Assembly", klass->name) == 0) {
		MonoReflectionAssembly *rassembly = (MonoReflectionAssembly*)obj;
		cinfo = mono_custom_attrs_from_assembly (rassembly->assembly);
	} else if (strcmp ("MonoProperty", klass->name) == 0) {
		MonoReflectionProperty *rprop = (MonoReflectionProperty*)obj;
		cinfo = mono_custom_attrs_from_property (rprop->klass, rprop->property);
	} else if (strcmp ("MonoEvent", klass->name) == 0) {
		MonoReflectionEvent *revent = (MonoReflectionEvent*)obj;
		cinfo = mono_custom_attrs_from_event (revent->klass, revent->event);
	} else if (strcmp ("MonoField", klass->name) == 0) {
		MonoReflectionField *rfield = (MonoReflectionField*)obj;
		cinfo = mono_custom_attrs_from_field (rfield->klass, rfield->field);
	} else if ((strcmp ("MonoMethod", klass->name) == 0) || (strcmp ("MonoCMethod", klass->name) == 0)) {
		MonoReflectionMethod *rmethod = (MonoReflectionMethod*)obj;
		cinfo = mono_custom_attrs_from_method (rmethod->method);
	} else if (strcmp ("ParameterInfo", klass->name) == 0) {
		MonoReflectionParameter *param = (MonoReflectionParameter*)obj;
		MonoReflectionMethod *rmethod = (MonoReflectionMethod*)param->MemberImpl;
		cinfo = mono_custom_attrs_from_param (rmethod->method, param->PositionImpl + 1);
	} else if (strcmp ("AssemblyBuilder", klass->name) == 0) {
		MonoReflectionAssemblyBuilder *assemblyb = (MonoReflectionAssemblyBuilder*)obj;
		cinfo = mono_custom_attrs_from_builders (assemblyb->assembly.assembly->image, assemblyb->cattrs);
	} else { /* handle other types here... */
		g_error ("get custom attrs not yet supported for %s", klass->name);
	}

	if (cinfo) {
		result = mono_custom_attrs_construct (cinfo);
	} else {
		klass = mono_class_from_name (mono_defaults.corlib, "System", "Attribute");
		result = mono_array_new (mono_domain_get (), klass, 0);
	}

	return result;
}

static MonoMethodSignature*
parameters_to_signature (MonoArray *parameters) {
	MonoMethodSignature *sig;
	int count, i;

	count = parameters? mono_array_length (parameters): 0;

	sig = g_malloc0 (sizeof (MonoMethodSignature) + sizeof (MonoType*) * count);
	sig->param_count = count;
	sig->sentinelpos = -1; /* FIXME */
	for (i = 0; i < count; ++i) {
		MonoReflectionType *pt = mono_array_get (parameters, MonoReflectionType*, i);
		sig->params [i] = pt->type;
	}
	return sig;
}

static MonoMethodSignature*
ctor_builder_to_signature (MonoReflectionCtorBuilder *ctor) {
	MonoMethodSignature *sig;

	sig = parameters_to_signature (ctor->parameters);
	sig->hasthis = ctor->attrs & METHOD_ATTRIBUTE_STATIC? 0: 1;
	sig->ret = &mono_defaults.void_class->byval_arg;
	return sig;
}

static MonoMethodSignature*
method_builder_to_signature (MonoReflectionMethodBuilder *method) {
	MonoMethodSignature *sig;

	sig = parameters_to_signature (method->parameters);
	sig->hasthis = method->attrs & METHOD_ATTRIBUTE_STATIC? 0: 1;
	sig->ret = method->rtype? method->rtype->type: &mono_defaults.void_class->byval_arg;
	sig->generic_param_count = method->generic_params ? mono_array_length (method->generic_params) : 0;
	return sig;
}

static void
get_prop_name_and_type (MonoObject *prop, char **name, MonoType **type)
{
	MonoClass *klass = mono_object_class (prop);
	if (strcmp (klass->name, "PropertyBuilder") == 0) {
		MonoReflectionPropertyBuilder *pb = (MonoReflectionPropertyBuilder *)prop;
		*name = mono_string_to_utf8 (pb->name);
		*type = pb->type->type;
	} else {
		MonoReflectionProperty *p = (MonoReflectionProperty *)prop;
		*name = g_strdup (p->property->name);
		if (p->property->get)
			*type = p->property->get->signature->ret;
		else
			*type = p->property->set->signature->params [p->property->set->signature->param_count - 1];
	}
}

static void
get_field_name_and_type (MonoObject *field, char **name, MonoType **type)
{
	MonoClass *klass = mono_object_class (field);
	if (strcmp (klass->name, "FieldBuilder") == 0) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder *)field;
		*name = mono_string_to_utf8 (fb->name);
		*type = fb->type->type;
	} else {
		MonoReflectionField *f = (MonoReflectionField *)field;
		*name = g_strdup (f->field->name);
		*type = f->field->type;
	}
}

static char*
type_get_qualified_name (MonoType *type, MonoAssembly *ass) {
	char *name, *result;
	MonoClass *klass;
	MonoAssembly *ta;

	name = mono_type_get_name (type);
	klass = my_mono_class_from_mono_type (type);
	ta = klass->image->assembly;
	if (ta == ass || klass->image == mono_defaults.corlib)
		return name;

	/* missing public key */
	result = g_strdup_printf ("%s, %s, Version=%d.%d.%d.%d, Culture=%s",
		name, ta->aname.name,
		ta->aname.major, ta->aname.minor, ta->aname.build, ta->aname.revision,
		ta->aname.culture && *ta->aname.culture? ta->aname.culture: "neutral");
	g_free (name);
	return result;
}

static void
encode_cattr_value (char *buffer, char *p, char **retbuffer, char **retp, guint32 *buflen, MonoType *type, MonoObject *arg)
{
	char *argval;
	MonoTypeEnum simple_type;
	
	if ((p-buffer) + 10 >= *buflen) {
		char *newbuf;
		*buflen *= 2;
		newbuf = g_realloc (buffer, *buflen);
		p = newbuf + (p-buffer);
		buffer = newbuf;
	}
	argval = ((char*)arg + sizeof (MonoObject));
	simple_type = type->type;
handle_enum:
	switch (simple_type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
		*p++ = *argval;
		break;
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
		swap_with_size (p, argval, 2, 1);
		p += 2;
		break;
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_R4:
		swap_with_size (p, argval, 4, 1);
		p += 4;
		break;
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
	case MONO_TYPE_R8:
		swap_with_size (p, argval, 8, 1);
		p += 8;
		break;
	case MONO_TYPE_VALUETYPE:
		if (type->data.klass->enumtype) {
			simple_type = type->data.klass->enum_basetype->type;
			goto handle_enum;
		} else {
			g_warning ("generic valutype %s not handled in custom attr value decoding", type->data.klass->name);
		}
		break;
	case MONO_TYPE_STRING: {
		char *str;
		guint32 slen;
		if (!arg) {
			*p++ = 0xFF;
			break;
		}
		str = mono_string_to_utf8 ((MonoString*)arg);
		slen = strlen (str);
		if ((p-buffer) + 10 + slen >= *buflen) {
			char *newbuf;
			*buflen *= 2;
			*buflen += slen;
			newbuf = g_realloc (buffer, *buflen);
			p = newbuf + (p-buffer);
			buffer = newbuf;
		}
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
		break;
	}
	case MONO_TYPE_CLASS: {
		char *str;
		guint32 slen;
		MonoClass *k;
		if (!arg) {
			*p++ = 0xFF;
			break;
		}
		k = mono_object_class (arg);
		if (!mono_object_isinst (arg, mono_defaults.monotype_class) &&
				(strcmp (k->name, "TypeBuilder") || strcmp (k->name_space, "System.Reflection.Emit")))
			g_error ("only types allowed, not %s.%s", k->name_space, k->name);
handle_type:
		str = type_get_qualified_name (((MonoReflectionType*)arg)->type, NULL);
		slen = strlen (str);
		if ((p-buffer) + 10 + slen >= *buflen) {
			char *newbuf;
			*buflen *= 2;
			*buflen += slen;
			newbuf = g_realloc (buffer, *buflen);
			p = newbuf + (p-buffer);
			buffer = newbuf;
		}
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
		break;
	}
	case MONO_TYPE_SZARRAY: {
		int len, i;
		MonoClass *eclass;

		if (!arg) {
			*p++ = 0xff; *p++ = 0xff; *p++ = 0xff; *p++ = 0xff;
			break;
		}
		len = mono_array_length ((MonoArray*)arg);
		*p++ = len & 0xff;
		*p++ = (len >> 8) & 0xff;
		*p++ = (len >> 16) & 0xff;
		*p++ = (len >> 24) & 0xff;
		*retp = p;
		*retbuffer = buffer;
		eclass = type->data.klass;
		for (i = 0; i < len; ++i) {
			encode_cattr_value (buffer, p, &buffer, &p, buflen, &eclass->byval_arg, mono_array_get ((MonoArray*)arg, MonoObject*, i));
		}
		break;
	}
	/* it may be a boxed value or a Type */
	case MONO_TYPE_OBJECT: {
		MonoClass *klass = mono_object_class (arg);
		char *str;
		guint32 slen;
		
		if (mono_object_isinst (arg, mono_defaults.monotype_class)) {
			*p++ = 0x50;
			goto handle_type;
		} else if (klass->enumtype) {
			*p++ = 0x55;
		} else if (klass == mono_defaults.string_class) {
			simple_type = MONO_TYPE_STRING;
			*p++ = 0x0E;
			goto handle_enum;
		} else if (klass->byval_arg.type >= MONO_TYPE_BOOLEAN && klass->byval_arg.type <= MONO_TYPE_R8) {
			*p++ = simple_type = klass->byval_arg.type;
			goto handle_enum;
		} else {
			g_error ("unhandled type in custom attr");
		}
		str = type_get_qualified_name (klass->enum_basetype, NULL);
		slen = strlen (str);
		if ((p-buffer) + 10 + slen >= *buflen) {
			char *newbuf;
			*buflen *= 2;
			*buflen += slen;
			newbuf = g_realloc (buffer, *buflen);
			p = newbuf + (p-buffer);
			buffer = newbuf;
		}
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
		simple_type = klass->enum_basetype->type;
		goto handle_enum;
	}
	default:
		g_error ("type 0x%02x not yet supported in custom attr encoder", simple_type);
	}
	*retp = p;
	*retbuffer = buffer;
}

/*
 * mono_reflection_get_custom_attrs_blob:
 * @ctor: custom attribute constructor
 * @ctorArgs: arguments o the constructor
 * @properties:
 * @propValues:
 * @fields:
 * @fieldValues:
 * 
 * Creates the blob of data that needs to be saved in the metadata and that represents
 * the custom attributed described by @ctor, @ctorArgs etc.
 * Returns: a Byte array representing the blob of data.
 */
MonoArray*
mono_reflection_get_custom_attrs_blob (MonoObject *ctor, MonoArray *ctorArgs, MonoArray *properties, MonoArray *propValues, MonoArray *fields, MonoArray* fieldValues) 
{
	MonoArray *result;
	MonoMethodSignature *sig;
	MonoObject *arg;
	char *buffer, *p;
	guint32 buflen, i;

	MONO_ARCH_SAVE_REGS;

	if (strcmp (ctor->vtable->klass->name, "MonoCMethod")) {
		sig = ctor_builder_to_signature ((MonoReflectionCtorBuilder*)ctor);
	} else {
		sig = ((MonoReflectionMethod*)ctor)->method->signature;
	}
	g_assert (mono_array_length (ctorArgs) == sig->param_count);
	buflen = 256;
	p = buffer = g_malloc (buflen);
	/* write the prolog */
	*p++ = 1;
	*p++ = 0;
	for (i = 0; i < sig->param_count; ++i) {
		arg = mono_array_get (ctorArgs, MonoObject*, i);
		encode_cattr_value (buffer, p, &buffer, &p, &buflen, sig->params [i], arg);
	}
	i = 0;
	if (properties)
		i += mono_array_length (properties);
	if (fields)
		i += mono_array_length (fields);
	*p++ = i & 0xff;
	*p++ = (i >> 8) & 0xff;
	if (properties) {
		MonoObject *prop;
		for (i = 0; i < mono_array_length (properties); ++i) {
			MonoType *ptype;
			char *pname;
			int len;
			
			prop = mono_array_get (properties, gpointer, i);
			get_prop_name_and_type (prop, &pname, &ptype);
			*p++ = 0x54; /* PROPERTY signature */

			/* Preallocate a large enough buffer */
			if (ptype->type == MONO_TYPE_VALUETYPE && ptype->data.klass->enumtype) {
				char *str = type_get_qualified_name (ptype, NULL);
				len = strlen (str);
				g_free (str);
			}
			else
				len = 0;
			len += strlen (pname);

			if ((p-buffer) + 20 + len >= buflen) {
				char *newbuf;
				buflen *= 2;
				buflen += len;
				newbuf = g_realloc (buffer, buflen);
				p = newbuf + (p-buffer);
				buffer = newbuf;
			}

			if (ptype->type == MONO_TYPE_VALUETYPE && ptype->data.klass->enumtype) {
				char *str = type_get_qualified_name (ptype, NULL);
				int slen = strlen (str);

				*p++ = 0x55;
				/*
				 * This seems to be optional...
				 * *p++ = 0x80;
				 */
				mono_metadata_encode_value (slen, p, &p);
				memcpy (p, str, slen);
				p += slen;
				g_free (str);
			} else {
				mono_metadata_encode_value (ptype->type, p, &p);
			}
			len = strlen (pname);
			mono_metadata_encode_value (len, p, &p);
			memcpy (p, pname, len);
			p += len;
			encode_cattr_value (buffer, p, &buffer, &p, &buflen, ptype, (MonoObject*)mono_array_get (propValues, gpointer, i));
			g_free (pname);
		}
	}

	if (fields) {
		MonoObject *field;
		for (i = 0; i < mono_array_length (fields); ++i) {
			MonoType *ftype;
			char *fname;
			int len;
			
			field = mono_array_get (fields, gpointer, i);
			get_field_name_and_type (field, &fname, &ftype);
			*p++ = 0x53; /* FIELD signature */
			if (ftype->type == MONO_TYPE_VALUETYPE && ftype->data.klass->enumtype) {
				char *str = type_get_qualified_name (ftype, NULL);
				int slen = strlen (str);
				if ((p-buffer) + 10 + slen >= buflen) {
					char *newbuf;
					buflen *= 2;
					buflen += slen;
					newbuf = g_realloc (buffer, buflen);
					p = newbuf + (p-buffer);
					buffer = newbuf;
				}
				*p++ = 0x55;
				/*
				 * This seems to be optional...
				 * *p++ = 0x80;
				 */
				mono_metadata_encode_value (slen, p, &p);
				memcpy (p, str, slen);
				p += slen;
				g_free (str);
			} else {
				mono_metadata_encode_value (ftype->type, p, &p);
			}
			len = strlen (fname);
			mono_metadata_encode_value (len, p, &p);
			memcpy (p, fname, len);
			p += len;
			encode_cattr_value (buffer, p, &buffer, &p, &buflen, ftype, (MonoObject*)mono_array_get (fieldValues, gpointer, i));
			g_free (fname);
		}
	}

	g_assert (p - buffer <= buflen);
	buflen = p - buffer;
	result = mono_array_new (mono_domain_get (), mono_defaults.byte_class, buflen);
	p = mono_array_addr (result, char, 0);
	memcpy (p, buffer, buflen);
	g_free (buffer);
	if (strcmp (ctor->vtable->klass->name, "MonoCMethod"))
		g_free (sig);
	return result;
}

/*
 * mono_reflection_setup_internal_class:
 * @tb: a TypeBuilder object
 *
 * Creates a MonoClass that represents the TypeBuilder.
 * This is a trick that lets us simplify a lot of reflection code
 * (and will allow us to support Build and Run assemblies easier).
 */
void
mono_reflection_setup_internal_class (MonoReflectionTypeBuilder *tb)
{
	MonoClass *klass, *parent;

	MONO_ARCH_SAVE_REGS;

	klass = g_new0 (MonoClass, 1);

	klass->image = &tb->module->dynamic_image->image;

	if (tb->parent) {
		/* check so we can compile corlib correctly */
		if (strcmp (mono_object_class (tb->parent)->name, "TypeBuilder") == 0) {
			/* mono_class_setup_mono_type () guaranteess type->data.klass is valid */
			parent = tb->parent->type->data.klass;
		} else 
			parent = my_mono_class_from_mono_type (tb->parent->type);
	} else
		parent = NULL;
	
	klass->inited = 1; /* we lie to the runtime */
	klass->name = mono_string_to_utf8 (tb->name);
	klass->name_space = mono_string_to_utf8 (tb->nspace);
	klass->type_token = MONO_TOKEN_TYPE_DEF | tb->table_idx;
	klass->flags = tb->attrs;

	klass->element_class = klass;
	klass->reflection_info = tb; /* need to pin. */

	/* Put into cache so mono_class_get () will find it */
	mono_image_add_to_name_cache (klass->image, klass->name_space, klass->name,
								  tb->table_idx);

	mono_g_hash_table_insert (tb->module->dynamic_image->tokens,
							  GUINT_TO_POINTER (MONO_TOKEN_TYPE_DEF | tb->table_idx),
							  tb);

	if (parent != NULL)
		mono_class_setup_parent (klass, parent);
	else if (strcmp (klass->name, "Object") == 0 && strcmp (klass->name_space, "System") == 0) {
		const char *old_n = klass->name;
		/* trick to get relative numbering right when compiling corlib */
		klass->name = "BuildingObject";
		mono_class_setup_parent (klass, mono_defaults.object_class);
		klass->name = old_n;
	}
	mono_class_setup_mono_type (klass);

	mono_class_setup_supertypes (klass);

	/*
	 * FIXME: handle interfaces.
	 */

	tb->type.type = &klass->byval_arg;

	if (tb->nesting_type) {
		g_assert (tb->nesting_type->type);
		klass->nested_in = mono_class_from_mono_type (tb->nesting_type->type);
	}

	/*g_print ("setup %s as %s (%p)\n", klass->name, ((MonoObject*)tb)->vtable->klass->name, tb);*/
}

/*
 * mono_reflection_setup_generic_class:
 * @tb: a TypeBuilder object
 *
 * Setup the generic class after all generic parameters have been added.
 */
void
mono_reflection_setup_generic_class (MonoReflectionTypeBuilder *tb)
{
	MonoClass *klass;
	int count, i;

	MONO_ARCH_SAVE_REGS;

	klass = my_mono_class_from_mono_type (tb->type.type);

	count = tb->generic_params ? mono_array_length (tb->generic_params) : 0;

	if (klass->gen_params || (count == 0))
		return;

	klass->num_gen_params = count;
	klass->gen_params = g_new0 (MonoGenericParam, count);

	for (i = 0; i < count; i++) {
		MonoReflectionGenericParam *gparam = mono_array_get (tb->generic_params, gpointer, i);
		klass->gen_params [i] = *gparam->param;
	}

	ensure_runtime_vtable (klass);
}

/*
 * mono_reflection_create_internal_class:
 * @tb: a TypeBuilder object
 *
 * Actually create the MonoClass that is associated with the TypeBuilder.
 */
void
mono_reflection_create_internal_class (MonoReflectionTypeBuilder *tb)
{
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	klass = my_mono_class_from_mono_type (tb->type.type);

	if (klass->enumtype && klass->enum_basetype == NULL) {
		MonoReflectionFieldBuilder *fb;

		g_assert (tb->fields != NULL);
		g_assert (mono_array_length (tb->fields) >= 1);

		fb = mono_array_get (tb->fields, MonoReflectionFieldBuilder*, 0);

		klass->enum_basetype = fb->type->type;
		klass->element_class = my_mono_class_from_mono_type (klass->enum_basetype);
		if (!klass->element_class)
			klass->element_class = mono_class_from_mono_type (klass->enum_basetype);
		klass->instance_size = klass->element_class->instance_size;
		klass->size_inited = 1;
		/* 
		 * this is almost safe to do with enums and it's needed to be able
		 * to create objects of the enum type (for use in SetConstant).
		 */
		/* FIXME: Does this mean enums can't have method overrides ? */
		mono_class_setup_vtable (klass, NULL, 0);
	}
}

static MonoMarshalSpec*
mono_marshal_spec_from_builder (MonoAssembly *assembly,
								MonoReflectionMarshal *minfo)
{
	MonoMarshalSpec *res;

	res = g_new0 (MonoMarshalSpec, 1);
	res->native = minfo->type;

	switch (minfo->type) {
	case MONO_NATIVE_LPARRAY:
		res->data.array_data.elem_type = minfo->eltype;
		res->data.array_data.param_num = 0; /* Not yet */
		res->data.array_data.num_elem = minfo->count;
		break;

	case MONO_NATIVE_BYVALTSTR:
	case MONO_NATIVE_BYVALARRAY:
		res->data.array_data.num_elem = minfo->count;
		break;

	case MONO_NATIVE_CUSTOM:
		if (minfo->marshaltyperef)
			res->data.custom_data.custom_name =
				type_get_qualified_name (minfo->marshaltyperef->type, 
										 assembly);
		if (minfo->mcookie)
			res->data.custom_data.cookie = mono_string_to_utf8 (minfo->mcookie);
		break;

	default:
		break;
	}

	return res;
}

static MonoMethod*
reflection_methodbuilder_to_mono_method (MonoClass *klass,
										 ReflectionMethodBuilder *rmb,
										 MonoMethodSignature *sig)
{
	MonoMethod *m;
	MonoMethodNormal *pm;
	MonoMarshalSpec **specs;
	MonoReflectionMethodAux *method_aux;
	int i;

	if ((rmb->attrs & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
	    (rmb->iattrs & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		m = (MonoMethod *)g_new0 (MonoMethodPInvoke, 1);
	else 
		m = (MonoMethod *)g_new0 (MonoMethodNormal, 1);

	pm = (MonoMethodNormal*)m;

	m->slot = -1;
	m->flags = rmb->attrs;
	m->iflags = rmb->iattrs;
	m->name = mono_string_to_utf8 (rmb->name);
	m->klass = klass;
	m->signature = sig;
	m->token = MONO_TOKEN_METHOD_DEF | (*rmb->table_idx);

	if (m->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
		if (klass == mono_defaults.string_class && !strcmp (m->name, ".ctor"))
			m->string_ctor = 1;

		m->signature->pinvoke = 1;
	} else if (m->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
		/* TODO */
		m->signature->pinvoke = 1;
		return m;
	} else if (!m->klass->dummy && 
			   !(m->flags & METHOD_ATTRIBUTE_ABSTRACT) &&
			   !(m->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME)) {
		MonoMethodHeader *header;
		guint32 code_size;
		gint32 max_stack, i;
		gint32 num_locals = 0;
		gint32 num_clauses = 0;
		guint8 *code;

		if (rmb->ilgen) {
			code = mono_array_addr (rmb->ilgen->code, guint8, 0);
			code_size = rmb->ilgen->code_len;
			max_stack = rmb->ilgen->max_stack;
			num_locals = rmb->ilgen->locals ? mono_array_length (rmb->ilgen->locals) : 0;
			if (rmb->ilgen->ex_handlers)
				num_clauses = method_count_clauses (rmb->ilgen);
		} else {
			if (rmb->code) {
				code = mono_array_addr (rmb->code, guint8, 0);
				code_size = mono_array_length (rmb->code);
				/* we probably need to run a verifier on the code... */
				max_stack = 8; 
			}
			else {
				code = NULL;
				code_size = 0;
				max_stack = 8;
			}
		}

		header = g_malloc0 (sizeof (MonoMethodHeader) + 
							(num_locals - MONO_ZERO_LEN_ARRAY) * sizeof (MonoType*));
		header->code_size = code_size;
		header->code = g_malloc (code_size);
		memcpy ((char*)header->code, code, code_size);
		header->max_stack = max_stack;
		header->init_locals = rmb->init_locals;
		header->num_locals = num_locals;

		for (i = 0; i < num_locals; ++i) {
			MonoReflectionLocalBuilder *lb = 
				mono_array_get (rmb->ilgen->locals, 
								MonoReflectionLocalBuilder*, i);

			header->locals [i] = g_new0 (MonoType, 1);
			memcpy (header->locals [i], lb->type->type, sizeof (MonoType));
		}

		header->num_clauses = num_clauses;
		if (num_clauses) {
			header->clauses = method_encode_clauses ((MonoDynamicImage*)klass->image,
													 rmb->ilgen,
													 num_clauses);
		}

		pm->header = header;
	}

	method_aux = g_new0 (MonoReflectionMethodAux, 1);

	/* Parameter names */
	if (rmb->parameters) {
		method_aux->param_names = g_new0 (char *, m->signature->param_count);
		for (i = 0; i < m->signature->param_count; ++i) {
			MonoReflectionParamBuilder *pb;
			if ((pb = mono_array_get (rmb->parameters, MonoReflectionParamBuilder*, i))) {
				if (pb->name)
					method_aux->param_names [i] = mono_string_to_utf8 (pb->name);
			}
		}
	}

	/* Parameter marshalling */
	specs = NULL;
	if (rmb->pinfo)		
		for (i = 0; i < mono_array_length (rmb->pinfo); ++i) {
			MonoReflectionParamBuilder *pb;
			if ((pb = mono_array_get (rmb->pinfo, MonoReflectionParamBuilder*, i))) {
				if (pb->marshal_info) {
					if (specs == NULL)
						specs = g_new0 (MonoMarshalSpec*, sig->param_count + 1);
					specs [pb->position] = 
						mono_marshal_spec_from_builder (klass->image->assembly, pb->marshal_info);
				}
			}
		}
	if (specs != NULL)
		method_aux->param_marshall = specs;

	mono_g_hash_table_insert (((MonoDynamicImage*)klass->image)->method_aux_hash, m, method_aux);

	return m;
}	

static MonoMethod*
ctorbuilder_to_mono_method (MonoClass *klass, MonoReflectionCtorBuilder* mb)
{
	ReflectionMethodBuilder rmb;
	MonoMethodSignature *sig;

	sig = ctor_builder_to_signature (mb);

	reflection_methodbuilder_from_ctor_builder (&rmb, mb);

	mb->mhandle = reflection_methodbuilder_to_mono_method (klass, &rmb, sig);
	mono_save_custom_attrs (klass->image, mb->mhandle, mb->cattrs);

	if (!((MonoDynamicImage*)(MonoDynamicImage*)klass->image)->save) {
		/* ilgen is no longer needed */
		mb->ilgen = NULL;
	}

	return mb->mhandle;
}

static MonoMethod*
methodbuilder_to_mono_method (MonoClass *klass, MonoReflectionMethodBuilder* mb)
{
	ReflectionMethodBuilder rmb;
	MonoMethodSignature *sig;

	sig = method_builder_to_signature (mb);

	reflection_methodbuilder_from_method_builder (&rmb, mb);

	mb->mhandle = reflection_methodbuilder_to_mono_method (klass, &rmb, sig);
	mono_save_custom_attrs (klass->image, mb->mhandle, mb->cattrs);

	if (!((MonoDynamicImage*)(MonoDynamicImage*)klass->image)->save) {
		/* ilgen is no longer needed */
		mb->ilgen = NULL;
	}
	return mb->mhandle;
}

static MonoClassField*
fieldbuilder_to_mono_class_field (MonoClass *klass, MonoReflectionFieldBuilder* fb)
{
	MonoClassField *field;
	const char *p, *p2;
	guint32 len, idx;

	if (fb->handle)
		return fb->handle;

	field = g_new0 (MonoClassField, 1);

	field->name = mono_string_to_utf8 (fb->name);
	if (fb->attrs) {
		/* FIXME: handle type modifiers */
		field->type = g_memdup (fb->type->type, sizeof (MonoType));
		field->type->attrs = fb->attrs;
	} else {
		field->type = fb->type->type;
	}
	if ((fb->attrs & FIELD_ATTRIBUTE_HAS_FIELD_RVA) && fb->rva_data)
		field->data = mono_array_addr (fb->rva_data, char, 0);
	if (fb->offset != -1)
		field->offset = fb->offset;
	field->parent = klass;
	fb->handle = field;
	mono_save_custom_attrs (klass->image, field, fb->cattrs);

	if (fb->def_value) {
		MonoDynamicImage *assembly = (MonoDynamicImage*)klass->image;
		field->type->attrs |= FIELD_ATTRIBUTE_HAS_DEFAULT;
		field->def_value = g_new0 (MonoConstant, 1);
		idx = encode_constant (assembly, fb->def_value, &field->def_value->type);
		/* Copy the data from the blob since it might get realloc-ed */
		p = assembly->blob.data + idx;
		len = mono_metadata_decode_blob_size (p, &p2);
		len += p2 - p;
		field->def_value->value = g_malloc (len);
		memcpy (field->def_value->value, p, len);
	}

	return field;
}

static MonoReflectionInflatedMethod*
inflated_method_get_object (MonoDomain *domain, MonoMethod *method, MonoReflectionMethod *declaring,
			    MonoGenericInst *ginst)
{
	const char *cname;
	MonoClass *klass, *refclass;
	MonoReflectionInflatedMethod *ret;

	refclass = method->klass;

	CHECK_OBJECT (MonoReflectionInflatedMethod *, method, refclass);
	if (*method->name == '.' && (strcmp (method->name, ".ctor") == 0 || strcmp (method->name, ".cctor") == 0))
		cname = "MonoInflatedCtor";
	else
		cname = "MonoInflatedMethod";
	klass = mono_class_from_name (mono_defaults.corlib, "System.Reflection", cname);
	g_assert (klass);

	ret = (MonoReflectionInflatedMethod*)mono_object_new (domain, klass);
	ret->rmethod.method = method;
	ret->rmethod.name = mono_string_new (domain, method->name);
	ret->rmethod.reftype = mono_type_get_object (domain, &refclass->byval_arg);
	ret->declaring = declaring;
	ret->ginst = ginst;
	CACHE_OBJECT (method, ret, refclass);
	return ret;
}

MonoReflectionGenericInst*
mono_reflection_bind_generic_parameters (MonoReflectionType *type, MonoArray *types)
{
	static MonoClass *System_Reflection_MonoGenericInst;
	MonoType *geninst;
	MonoGenericInst *ginst;
	MonoClass *klass, *iklass;
	MonoReflectionTypeBuilder *tb = NULL;
	MonoReflectionGenericInst *res, *parent = NULL;
	int i;

	klass = mono_class_from_mono_type (type->type);
	if (klass->num_gen_params != mono_array_length (types))
		return NULL;

	if (!System_Reflection_MonoGenericInst) {
		System_Reflection_MonoGenericInst = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "MonoGenericInst");
		g_assert (System_Reflection_MonoGenericInst);
	}

	if (klass->wastypebuilder && klass->reflection_info) {
		tb = klass->reflection_info;

		if (tb->parent)
			parent = mono_reflection_bind_generic_parameters (tb->parent, types);
	}

	geninst = g_new0 (MonoType, 1);
	geninst->type = MONO_TYPE_GENERICINST;
	geninst->data.generic_inst = ginst = g_new0 (MonoGenericInst, 1);
	ginst->generic_type = &klass->byval_arg;
	ginst->type_argc = klass->num_gen_params;
	ginst->type_argv = g_new0 (MonoType *, klass->num_gen_params);
	for (i = 0; i < klass->num_gen_params; ++i) {
		MonoReflectionType *garg = mono_array_get (types, gpointer, i);
		ginst->type_argv [i] = garg->type;
	}

	iklass = mono_class_from_generic (geninst, FALSE);

	mono_class_setup_parent (iklass, parent ? parent->klass : mono_defaults.object_class);
	mono_class_setup_mono_type (iklass);

	res = (MonoReflectionGenericInst *)mono_object_new (mono_object_domain (type), System_Reflection_MonoGenericInst);

	res->type.type = iklass->generic_inst;
	res->klass = iklass;
	res->parent = parent;
	res->generic_type = type;

	return res;
}

MonoReflectionInflatedMethod*
mono_reflection_bind_generic_method_parameters (MonoReflectionMethod *rmethod, MonoArray *types)
{
	MonoMethod *method, *inflated;
	MonoReflectionMethodBuilder *mb = NULL;
	MonoGenericInst *ginst;
	int count, i;

	MONO_ARCH_SAVE_REGS;
	if (!strcmp (rmethod->object.vtable->klass->name, "MethodBuilder")) {
		MonoReflectionTypeBuilder *tb;
		MonoClass *klass;

		mb = (MonoReflectionMethodBuilder *) rmethod;
		tb = (MonoReflectionTypeBuilder *) mb->type;
		klass = mono_class_from_mono_type (tb->type.type);

		method = methodbuilder_to_mono_method (klass, mb);
	} else
		method = rmethod->method;

	count = method->signature->generic_param_count;
	if (count != mono_array_length (types))
		return NULL;

	ginst = g_new0 (MonoGenericInst, 1);
	ginst->generic_method = method;
	ginst->type_argc = count;
	ginst->type_argv = g_new0 (MonoType *, count);
	for (i = 0; i < count; i++) {
		MonoReflectionType *garg = mono_array_get (types, gpointer, i);
		ginst->type_argv [i] = garg->type;
	}

	inflated = mono_class_inflate_generic_method (method, ginst);

	return inflated_method_get_object (mono_object_domain (rmethod), inflated, rmethod, ginst);
}

MonoReflectionInflatedMethod*
mono_reflection_inflate_method_or_ctor (MonoReflectionGenericInst *declaring_type,
					MonoReflectionGenericInst *reflected_type,
					MonoObject *obj)
{
	MonoGenericInst *ginst, *type_ginst;
	MonoMethod *method, *inflated;
	MonoReflectionInflatedMethod *res;
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	klass = mono_class_from_mono_type (reflected_type->type.type);
	type_ginst = reflected_type->type.type->data.generic_inst;

	if (!strcmp (obj->vtable->klass->name, "MethodBuilder"))
		method = methodbuilder_to_mono_method (klass, (MonoReflectionMethodBuilder *) obj);
	else if (!strcmp (obj->vtable->klass->name, "ConstructorBuilder"))
		method = ctorbuilder_to_mono_method (klass, (MonoReflectionCtorBuilder *) obj);
	else if (!strcmp (obj->vtable->klass->name, "MonoMethod") ||
		 !strcmp (obj->vtable->klass->name, "MonoCMethod"))
		method = ((MonoReflectionMethod *) obj)->method;
	else
		g_assert_not_reached ();

	ginst = g_new0 (MonoGenericInst, 1);
	ginst->generic_method = method;
	ginst->generic_type = reflected_type->type.type;
	ginst->type_argc = type_ginst->type_argc;
	ginst->type_argv = type_ginst->type_argv;

	ginst->klass = mono_class_from_generic (ginst->generic_type, FALSE);

	inflated = mono_class_inflate_generic_method (method, ginst);

	res = inflated_method_get_object (
		mono_object_domain (reflected_type), inflated, (MonoReflectionMethod *) obj, ginst);

	res->declaring_type = declaring_type;
	res->reflected_type = reflected_type;

	return res;
}

MonoReflectionInflatedField*
mono_reflection_inflate_field (MonoReflectionGenericInst *declaring_type,
			       MonoReflectionGenericInst *reflected_type,
			       MonoObject *obj)
{
	static MonoClass *System_Reflection_MonoInflatedField;
	MonoGenericInst *ginst, *type_ginst;
	MonoClassField *field, *inflated;
	MonoReflectionInflatedField *res;
	MonoDomain *domain;
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	if (!System_Reflection_MonoInflatedField) {
		System_Reflection_MonoInflatedField = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "MonoInflatedField");
		g_assert (System_Reflection_MonoInflatedField);
	}

	klass = mono_class_from_mono_type (reflected_type->type.type);
	type_ginst = reflected_type->type.type->data.generic_inst;

	if (!strcmp (obj->vtable->klass->name, "FieldBuilder")) {
		field = fieldbuilder_to_mono_class_field (klass, (MonoReflectionFieldBuilder *) obj);
	} else if (!strcmp (obj->vtable->klass->name, "MonoField"))
		field = ((MonoReflectionField *) obj)->field;
	else
		g_assert_not_reached ();

	ginst = g_new0 (MonoGenericInst, 1);
	ginst->generic_type = reflected_type->type.type;
	ginst->type_argc = type_ginst->type_argc;
	ginst->type_argv = type_ginst->type_argv;

	inflated = g_new0 (MonoClassField, 1);
	*inflated = *field;
	inflated->type = mono_class_inflate_generic_type (field->type, ginst);

	domain = mono_object_domain (obj);

	res = (MonoReflectionInflatedField *)mono_object_new (domain, System_Reflection_MonoInflatedField);
	res->declaring = field;
	res->declaring_type = declaring_type;
	res->reflected_type = reflected_type;
	res->rfield.klass = klass;
	res->rfield.field = inflated;
	res->rfield.name = mono_string_new (domain, inflated->name);
	res->rfield.attrs = inflated->type->attrs;
	res->rfield.type = mono_type_get_object (domain, inflated->type);
	CACHE_OBJECT (inflated, res, field->parent);
	return res;
}

static void
ensure_runtime_vtable (MonoClass *klass)
{
	MonoReflectionTypeBuilder *tb = klass->reflection_info;
	int i, num, j, onum;
	MonoMethod **overrides;

	if (!tb || klass->wastypebuilder)
		return;
	if (klass->parent)
		ensure_runtime_vtable (klass->parent);

	num = tb->ctors? mono_array_length (tb->ctors): 0;
	num += tb->methods? mono_array_length (tb->methods): 0;
	klass->method.count = num;
	klass->methods = g_new (MonoMethod*, num);
	num = tb->ctors? mono_array_length (tb->ctors): 0;
	for (i = 0; i < num; ++i)
		klass->methods [i] = ctorbuilder_to_mono_method (klass, mono_array_get (tb->ctors, MonoReflectionCtorBuilder*, i));
	num = tb->methods? mono_array_length (tb->methods): 0;
	j = i;
	for (i = 0; i < num; ++i)
		klass->methods [j++] = methodbuilder_to_mono_method (klass, mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i));

	klass->wastypebuilder = TRUE;
	if (tb->interfaces) {
		klass->interface_count = mono_array_length (tb->interfaces);
		klass->interfaces = g_new (MonoClass*, klass->interface_count);
		for (i = 0; i < klass->interface_count; ++i) {
			MonoReflectionType *iface = mono_array_get (tb->interfaces, gpointer, i);
			klass->interfaces [i] = mono_class_from_mono_type (iface->type);
		}
	}

	if (klass->flags & TYPE_ATTRIBUTE_INTERFACE)
		for (i = 0; i < klass->method.count; ++i)
			klass->methods [i]->slot = i;

	/* Overrides */
	onum = 0;
	if (tb->methods) {
		for (i = 0; i < mono_array_length (tb->methods); ++i) {
			MonoReflectionMethodBuilder *mb = 
				mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i);
			if (mb->override_method)
				onum ++;
		}
	}

	overrides = (MonoMethod**)g_new0 (MonoMethod, onum * 2);

	if (tb->methods) {
		onum = 0;
		for (i = 0; i < mono_array_length (tb->methods); ++i) {
			MonoReflectionMethodBuilder *mb = 
				mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i);
			if (mb->override_method) {
				/* FIXME: What if 'override_method' is a MethodBuilder ? */
				overrides [onum * 2] = 
					mb->override_method->method;
				overrides [onum * 2 + 1] =
					mb->mhandle;

				g_assert (mb->mhandle);

				onum ++;
			}
		}
	}

	mono_class_setup_vtable (klass, overrides, onum);
}

static void
typebuilder_setup_fields (MonoClass *klass)
{
	MonoReflectionTypeBuilder *tb = klass->reflection_info;
	MonoReflectionFieldBuilder *fb;
	MonoClassField *field;
	const char *p, *p2;
	int i;
	guint32 len, idx;

	klass->field.count = tb->fields? mono_array_length (tb->fields): 0;
	klass->field.first = 0;
	klass->field.last = klass->field.count;

	if (!klass->field.count)
		return;
	
	klass->fields = g_new0 (MonoClassField, klass->field.count);

	for (i = 0; i < klass->field.count; ++i) {
		fb = mono_array_get (tb->fields, gpointer, i);
		field = &klass->fields [i];
		field->name = mono_string_to_utf8 (fb->name);
		if (fb->attrs) {
			/* FIXME: handle type modifiers */
			field->type = g_memdup (fb->type->type, sizeof (MonoType));
			field->type->attrs = fb->attrs;
		} else {
			field->type = fb->type->type;
		}
		if ((fb->attrs & FIELD_ATTRIBUTE_HAS_FIELD_RVA) && fb->rva_data)
			field->data = mono_array_addr (fb->rva_data, char, 0);
		if (fb->offset != -1)
			field->offset = fb->offset;
		field->parent = klass;
		fb->handle = field;
		mono_save_custom_attrs (klass->image, field, fb->cattrs);

		if (fb->def_value) {
			MonoDynamicImage *assembly = (MonoDynamicImage*)klass->image;
			field->type->attrs |= FIELD_ATTRIBUTE_HAS_DEFAULT;
			field->def_value = g_new0 (MonoConstant, 1);
			idx = encode_constant (assembly, fb->def_value, &field->def_value->type);
			/* Copy the data from the blob since it might get realloc-ed */
			p = assembly->blob.data + idx;
			len = mono_metadata_decode_blob_size (p, &p2);
			len += p2 - p;
			field->def_value->value = g_malloc (len);
			memcpy (field->def_value->value, p, len);
		}
	}
	mono_class_layout_fields (klass);
}

static void
typebuilder_setup_properties (MonoClass *klass)
{
	MonoReflectionTypeBuilder *tb = klass->reflection_info;
	MonoReflectionPropertyBuilder *pb;
	int i;

	klass->property.count = tb->properties ? mono_array_length (tb->properties) : 0;
	klass->property.first = 0;
	klass->property.last = klass->property.count;

	klass->properties = g_new0 (MonoProperty, klass->property.count);
	for (i = 0; i < klass->property.count; ++i) {
		pb = mono_array_get (tb->properties, MonoReflectionPropertyBuilder*, i);
		klass->properties [i].attrs = pb->attrs;
		klass->properties [i].name = mono_string_to_utf8 (pb->name);
		if (pb->get_method)
			klass->properties [i].get = pb->get_method->mhandle;
		if (pb->set_method)
			klass->properties [i].set = pb->set_method->mhandle;
	}
}

static void
typebuilder_setup_events (MonoClass *klass)
{
	MonoReflectionTypeBuilder *tb = klass->reflection_info;
	MonoReflectionEventBuilder *eb;
	int i, j;

	klass->event.count = tb->events ? mono_array_length (tb->events) : 0;
	klass->event.first = 0;
	klass->event.last = klass->event.count;

	klass->events = g_new0 (MonoEvent, klass->event.count);
	for (i = 0; i < klass->event.count; ++i) {
		eb = mono_array_get (tb->events, MonoReflectionEventBuilder*, i);
		klass->events [i].attrs = eb->attrs;
		klass->events [i].name = mono_string_to_utf8 (eb->name);
		if (eb->add_method)
			klass->events [i].add = eb->add_method->mhandle;
		if (eb->remove_method)
			klass->events [i].remove = eb->remove_method->mhandle;
		if (eb->raise_method)
			klass->events [i].raise = eb->raise_method->mhandle;

		if (eb->other_methods) {
			klass->events [i].other = g_new0 (MonoMethod*, mono_array_length (eb->other_methods));
			for (j = 0; j < mono_array_length (eb->other_methods); ++j) {
				MonoReflectionMethodBuilder *mb = 
					mono_array_get (eb->other_methods,
									MonoReflectionMethodBuilder*, j);
				klass->events [i].other [j] = mb->mhandle;
			}
		}
	}
}

MonoReflectionType*
mono_reflection_create_runtime_class (MonoReflectionTypeBuilder *tb)
{
	MonoClass *klass;
	MonoReflectionType* res;

	MONO_ARCH_SAVE_REGS;

	klass = my_mono_class_from_mono_type (tb->type.type);

	mono_save_custom_attrs (klass->image, klass, tb->cattrs);

	/*
	 * Fields to set in klass:
	 * the various flags: delegate/unicode/contextbound etc.
	 * nested_classes
	 */
	klass->flags = tb->attrs;
	klass->element_class = klass;

	if (!((MonoDynamicImage*)(MonoDynamicImage*)klass->image)->run)
		/* No need to fully construct the type */
		return mono_type_get_object (mono_object_domain (tb), &klass->byval_arg);

	/* enums are done right away */
	if (!klass->enumtype)
		ensure_runtime_vtable (klass);

	/* fields and object layout */
	if (klass->parent) {
		if (!klass->parent->size_inited)
			mono_class_init (klass->parent);
		klass->instance_size += klass->parent->instance_size;
		klass->class_size += klass->parent->class_size;
		klass->min_align = klass->parent->min_align;
	} else {
		klass->instance_size = sizeof (MonoObject);
		klass->min_align = 1;
	}

	/* FIXME: handle packing_size and instance_size */
	typebuilder_setup_fields (klass);

	typebuilder_setup_properties (klass);

	typebuilder_setup_events (klass);

	res = mono_type_get_object (mono_object_domain (tb), &klass->byval_arg);
	/* with enums res == tb: need to fix that. */
	if (!klass->enumtype)
		g_assert (res != (MonoReflectionType*)tb);
	return res;
}

MonoReflectionType *
mono_reflection_define_generic_parameter (MonoReflectionTypeBuilder *tb, MonoReflectionMethodBuilder *mb, guint32 index, MonoReflectionGenericParam *gparam)
{
	MonoClass *klass;
	MonoImage *image;
	MonoGenericParam *param;
	int count, i;

	param = gparam->param = g_new0 (MonoGenericParam, 1);

	if (mb)
		tb = (MonoReflectionTypeBuilder *) mb->type;

	image = (MonoImage*)tb->module->dynamic_image;

	param->method = NULL;
	param->name = mono_string_to_utf8 (gparam->name);
	param->num = index;

	count = gparam->constraints ? mono_array_length (gparam->constraints) : 0;
	param->constraints = g_new0 (MonoClass *, count + 1);
	for (i = 0; i < count; i++) {
		MonoReflectionType *constraint = mono_array_get (gparam->constraints, MonoReflectionType *, i);

		param->constraints [i] = mono_class_from_mono_type (constraint->type);
	}

	klass = mono_class_from_generic_parameter (param, image, mb != NULL);

	gparam->type = mono_type_get_object (mono_object_domain (tb), &klass->byval_arg);

	return gparam->type;
}

MonoArray *
mono_reflection_sighelper_get_signature_local (MonoReflectionSigHelper *sig)
{
	MonoDynamicImage *assembly = sig->module->dynamic_image;
	guint32 na = mono_array_length (sig->arguments);
	guint32 buflen, i;
	MonoArray *result;
	char *buf, *p;

	MONO_ARCH_SAVE_REGS;

	p = buf = g_malloc (10 + na * 10);

	mono_metadata_encode_value (0x07, p, &p);
	mono_metadata_encode_value (na, p, &p);
	for (i = 0; i < na; ++i) {
		MonoReflectionType *type = mono_array_get (sig->arguments, MonoReflectionType *, i);
		encode_reflection_type (assembly, type, p, &p);
	}

	buflen = p - buf;
	result = mono_array_new (mono_domain_get (), mono_defaults.byte_class, buflen);
	p = mono_array_addr (result, char, 0);
	memcpy (p, buf, buflen);
	g_free (buf);

	return result;
}

MonoArray *
mono_reflection_sighelper_get_signature_field (MonoReflectionSigHelper *sig)
{
	MonoDynamicImage *assembly = sig->module->dynamic_image;
	guint32 na = mono_array_length (sig->arguments);
	guint32 buflen, i;
	MonoArray *result;
	char *buf, *p;

	MONO_ARCH_SAVE_REGS;

	p = buf = g_malloc (10 + na * 10);

	mono_metadata_encode_value (0x06, p, &p);
	for (i = 0; i < na; ++i) {
		MonoReflectionType *type = mono_array_get (sig->arguments, MonoReflectionType *, i);
		encode_reflection_type (assembly, type, p, &p);
	}

	buflen = p - buf;
	result = mono_array_new (mono_domain_get (), mono_defaults.byte_class, buflen);
	p = mono_array_addr (result, char, 0);
	memcpy (p, buf, buflen);
	g_free (buf);

	return result;
}

/**
 * mono_reflection_lookup_dynamic_token:
 *
 *  Finish the Builder object pointed to by TOKEN and return the corresponding
 * runtime structure.
 */
gpointer
mono_reflection_lookup_dynamic_token (MonoImage *image, guint32 token)
{
	MonoDynamicImage *assembly = (MonoDynamicImage*)image;
	MonoObject *obj;
	gpointer result;

	obj = mono_g_hash_table_lookup (assembly->tokens, GUINT_TO_POINTER (token));
	g_assert (obj);

	if (strcmp (obj->vtable->klass->name, "String") == 0) {
		result = mono_string_intern ((MonoString*)obj);
		g_assert (result);
	}
	else if (strcmp (obj->vtable->klass->name, "MonoType") == 0) {
		MonoReflectionType *tb = (MonoReflectionType*)obj;
		result = mono_class_from_mono_type (tb->type);
		g_assert (result);
	}
	else if (strcmp (obj->vtable->klass->name, "MonoMethod") == 0) {
		result = ((MonoReflectionMethod*)obj)->method;
		g_assert (result);
	}
	else if (strcmp (obj->vtable->klass->name, "MonoCMethod") == 0) {
		result = ((MonoReflectionMethod*)obj)->method;
		g_assert (result);
	}
	else if (strcmp (obj->vtable->klass->name, "MethodBuilder") == 0) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder*)obj;
		result = mb->mhandle;
		if (!result) {
			/* Type is not yet created */
			MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)mb->type;

			mono_domain_try_type_resolve (mono_domain_get (), NULL, (MonoObject*)tb);

			/*
			 * Hopefully this has been filled in by calling CreateType() on the
			 * TypeBuilder.
			 */
			/**
			 * TODO: This won't work if the application finishes another 
			 * TypeBuilder instance instead of this one.
			 */
			result = mb->mhandle;
		}
	}
	else if (strcmp (obj->vtable->klass->name, "ConstructorBuilder") == 0) {
		MonoReflectionCtorBuilder *cb = (MonoReflectionCtorBuilder*)obj;

		result = cb->mhandle;
		if (!result) {
			MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)cb->type;

			mono_domain_try_type_resolve (mono_domain_get (), NULL, (MonoObject*)tb);
			result = cb->mhandle;
		}
	}
	else if (strcmp (obj->vtable->klass->name, "MonoField") == 0) {
		result = ((MonoReflectionField*)obj)->field;
		g_assert (result);
	}
	else if (strcmp (obj->vtable->klass->name, "FieldBuilder") == 0) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder*)obj;
		result = fb->handle;

		if (!result) {
			MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)fb->typeb;

			mono_domain_try_type_resolve (mono_domain_get (), NULL, (MonoObject*)tb);
			result = fb->handle;
		}
	}
	else if (strcmp (obj->vtable->klass->name, "TypeBuilder") == 0) {
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)obj;
		MonoClass *klass;

		klass = tb->type.type->data.klass;
		if (klass->wastypebuilder) {
			/* Already created */
			result = klass;
		}
		else {
			mono_domain_try_type_resolve (mono_domain_get (), NULL, (MonoObject*)tb);
			result = tb->type.type->data.klass;
			g_assert (result);
		}
	}
	else if (strcmp (obj->vtable->klass->name, "SignatureHelper") == 0) {
		MonoReflectionSigHelper *helper = (MonoReflectionSigHelper*)obj;
		MonoMethodSignature *sig;
		int nargs, i;

		if (helper->arguments)
			nargs = mono_array_length (helper->arguments);
		else
			nargs = 0;

		sig = mono_metadata_signature_alloc (image, nargs);
		sig->explicit_this = helper->call_conv & 64;
		sig->hasthis = helper->call_conv & 32;

		if (helper->call_conv == 0) /* unmanaged */
			sig->call_convention = helper->unmanaged_call_conv - 1;
		else
			if (helper->call_conv & 0x02)
				sig->call_convention = MONO_CALL_VARARG;
		else
			sig->call_convention = MONO_CALL_DEFAULT;

		sig->param_count = nargs;
		/* TODO: Copy type ? */
		sig->ret = helper->return_type->type;
		for (i = 0; i < nargs; ++i) {
			MonoReflectionType *rt = mono_array_get (helper->arguments, MonoReflectionType*, i);
			sig->params [i] = rt->type;
		}

		result = sig;
	}
	else {
		g_print (obj->vtable->klass->name);
		g_assert_not_reached ();
	}
	return result;
}





