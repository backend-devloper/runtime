/*
 * get.c: Functions to get stringified values from the metadata tables.
 *
 * Author:
 *   Miguel de Icaza (miguel@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include "meta.h"
#include "util.h"
#include "get.h"
#include <mono/metadata/class.h>
#include <mono/metadata/marshal.h>

extern gboolean substitute_with_mscorlib_p;

static char *
get_memberref_parent (MonoImage *m, guint32 mrp_token);

char *
get_typedef (MonoImage *m, int idx)
{
	guint32 cols [MONO_TYPEDEF_SIZE];
	const char *ns;

	mono_metadata_decode_row (&m->tables [MONO_TABLE_TYPEDEF], idx - 1, cols, MONO_TYPEDEF_SIZE);

	ns = mono_metadata_string_heap (m, cols [MONO_TYPEDEF_NAMESPACE]);
	return g_strdup_printf (
		"%s%s%s", ns, *ns?".":"",
		mono_metadata_string_heap (m, cols [MONO_TYPEDEF_NAME]));
}

char *
get_module (MonoImage *m, int idx)
{
	guint32 cols [MONO_MODULE_SIZE];
	
	/*
	 * There MUST BE only one module in the Module table
	 */
	g_assert (idx == 1);
	    
	mono_metadata_decode_row (&m->tables [MONO_TABLE_MODULE], idx - 1, cols, MONO_MODULE_SIZE);

	return g_strdup (mono_metadata_string_heap (m, cols [MONO_MODULE_NAME]));
}

char *
get_assemblyref (MonoImage *m, int idx)
{
	guint32 cols [MONO_ASSEMBLYREF_SIZE];
	
	mono_metadata_decode_row (&m->tables [MONO_TABLE_ASSEMBLYREF], idx - 1, cols, MONO_ASSEMBLYREF_SIZE);

	return g_strdup (mono_metadata_string_heap (m, cols [MONO_ASSEMBLYREF_NAME]));
}

/*
 *
 * Returns a string representing the ArrayShape (22.2.16).
 */
static const char *
get_array_shape (MonoImage *m, const char *ptr, char **result)
{
	GString *res = g_string_new ("[");
	guint32 rank, num_sizes, num_lo_bounds;
	guint32 *sizes = NULL, *lo_bounds = NULL;
	int i, r;
	char buffer [80];
	
	rank = mono_metadata_decode_value (ptr, &ptr);
	num_sizes = mono_metadata_decode_value (ptr, &ptr);

	if (num_sizes > 0)
		sizes = g_new (guint32, num_sizes);
	
	for (i = 0; i < num_sizes; i++)
		sizes [i] = mono_metadata_decode_value (ptr, &ptr);

	num_lo_bounds = mono_metadata_decode_value (ptr, &ptr);
	if (num_lo_bounds > 0)
		lo_bounds = g_new (guint32, num_lo_bounds);
	
	for (i = 0; i < num_lo_bounds; i++)
		lo_bounds [i] = mono_metadata_decode_value (ptr, &ptr);

	for (r = 0; r < rank; r++){
		if (r < num_sizes){
			if (r < num_lo_bounds){
				sprintf (buffer, "%d..%d", lo_bounds [r], lo_bounds [r] + sizes [r] - 1);
			} else {
				sprintf (buffer, "0..%d", sizes [r] - 1);
			}
		} else
			buffer [0] = 0;
		
		g_string_append (res, buffer);
		if ((r + 1) != rank)
			g_string_append (res, ", ");
	}
	g_string_append (res, "]");
	
	if (sizes)
		g_free (sizes);

	if (lo_bounds)
		g_free (lo_bounds);

	*result = res->str;
	g_string_free (res, FALSE);

	return ptr;
}

/**
 * get_typespec:
 * @m: metadata context
 * @blob_idx: index into the blob heap
 *
 * Returns the stringified representation of a TypeSpec signature (22.2.17)
 */
char *
get_typespec (MonoImage *m, guint32 idx)
{
	guint32 cols [MONO_TYPESPEC_SIZE];
	const char *ptr;
	char *s, *result;
	GString *res = g_string_new ("");
	int len;

	mono_metadata_decode_row (&m->tables [MONO_TABLE_TYPESPEC], idx-1, cols, MONO_TYPESPEC_SIZE);
	ptr = mono_metadata_blob_heap (m, cols [MONO_TYPESPEC_SIGNATURE]);
	len = mono_metadata_decode_value (ptr, &ptr);
	
	switch (*ptr++){
	case MONO_TYPE_PTR:
		ptr = get_custom_mod (m, ptr, &s);
		if (s){
			g_string_append (res, s);
			g_string_append_c (res, ' ');
			g_free (s);
		}
		
		if (*ptr == MONO_TYPE_VOID)
			g_string_append (res, "void");
		else {
			ptr = get_type (m, ptr, &s);
			if (s)
				g_string_append (res, s);
		}
		break;
		
	case MONO_TYPE_FNPTR:
		g_string_append (res, "FNPTR ");
		/*
		 * we assume MethodRefSig, as we do not know
		 * whether it is a MethodDefSig or a MethodRefSig.
		 */
		printf ("\n FNPTR:\n");
		
		hex_dump (ptr, 0, 40);
		break;
			
	case MONO_TYPE_ARRAY:
		ptr = get_type (m, ptr, &s);
		g_string_append (res, s);
		g_free (s);
		g_string_append_c (res, ' ');
		ptr = get_array_shape (m, ptr, &s);
		g_string_append (res, s);
		g_free (s);
		break;
		
	case MONO_TYPE_SZARRAY:
		ptr = get_custom_mod (m, ptr, &s);
		if (s){
			g_string_append (res, s);
			g_string_append_c (res, ' ');
			g_free (s);
		}
		ptr = get_type (m, ptr, &s);
		g_string_append (res, s);
		g_string_append (res, "[]");
		g_free (s);
	}

	result = res->str;
	g_string_free (res, FALSE);

	return result;
}

char *
get_typeref (MonoImage *m, int idx)
{
	guint32 cols [MONO_TYPEREF_SIZE];
	const char *s, *t;
	char *x, *ret;
	guint32 rs_idx, table;
	
	mono_metadata_decode_row (&m->tables [MONO_TABLE_TYPEREF], idx - 1, cols, MONO_TYPEREF_SIZE);

	t = mono_metadata_string_heap (m, cols [MONO_TYPEREF_NAME]);
	s = mono_metadata_string_heap (m, cols [MONO_TYPEREF_NAMESPACE]);

	rs_idx = cols [MONO_TYPEREF_SCOPE] >> RESOLTION_SCOPE_BITS;
	table = cols [MONO_TYPEREF_SCOPE] & RESOLTION_SCOPE_MASK;
	
	switch (table){
	case RESOLTION_SCOPE_MODULE: /* Module */
		x = get_module (m, rs_idx);
		ret = g_strdup_printf ("[%s] %s%s%s", x, s, *s?".":"", t);
		g_free (x);
		break;

	case RESOLTION_SCOPE_MODULEREF: /* ModuleRef */
		ret = g_strdup_printf ("TODO:TypeRef-ModuleRef (%s.%s)", s, t);
		break;
			      
	case RESOLTION_SCOPE_ASSEMBLYREF: /*
		 * AssemblyRef (ECMA docs claim it is 3, but it looks to
		 * me like it is 2 (tokens are prefixed with 0x23)
		 */
		x = get_assemblyref (m, rs_idx);
		ret = g_strdup_printf ("[%s]%s%s%s", x, s, *s?".":"", t);
		g_free (x);
		break;
		
	case RESOLTION_SCOPE_TYPEREF: /* TypeRef */
		x = get_typeref (m, rs_idx);
		ret =  g_strdup_printf ("%s/%s", x, t);
		g_free (x);
		break;
		
	default:
		ret = g_strdup_printf ("Unknown table in TypeRef %d", table);
	}

	return ret;
}

/**
 * get_typedef_or_ref:
 * @m: metadata context
 * @dor_token: def or ref encoded index
 *
 * Low two bits contain table to lookup from
 * high bits contain the index into the def or ref table
 *
 * Returns: a stringified version of the MethodDef or MethodRef
 * at (dor_token >> 2) 
 */
char *
get_typedef_or_ref (MonoImage *m, guint32 dor_token)
{
	char *temp = NULL, *s;
	int table, idx;

	/*
	 * low 2 bits contain encoding
	 */
	table = dor_token & TYPEDEFORREF_MASK;
	idx = dor_token >> TYPEDEFORREF_BITS;
	
	switch (table){
	case 0: /* TypeDef */
		temp = get_typedef (m, idx);
		s = g_strdup_printf ("%s", temp);
		break;
		
	case 1: /* TypeRef */
		temp = get_typeref (m, idx);
		s = g_strdup_printf ("%s", temp);
		break;
		
	case 2: /* TypeSpec */
		s = get_typespec (m, idx);
		break;

	default:
		g_error ("Unhandled encoding for typedef-or-ref coded index 0x%08x", dor_token);

	}
	
	if (temp)
		g_free (temp);

	return s;
}

/**
 * get_type_or_methdef
 * @m: metadata context
 * @dor_token: type or method def encoded index
 *
 * Low bit contains the table to lookup from
 * high bits contain the index into the type def or method def table
 *
 * Returns: a stringified version of the TypeOrMethodDef token
 */
char *
get_type_or_methdef (MonoImage *m, guint32 dor_token)
{
        if (dor_token & MONO_TYPEORMETHOD_METHOD) /* MethodDef */
		return get_methoddef (m, dor_token >> MONO_TYPEORMETHOD_BITS);
        else  /* TypeDef */
                return get_typedef (m, dor_token >> MONO_TYPEORMETHOD_BITS);
}

/** 
 * get_encoded_typedef_or_ref:
 * @m: metadata context 
 * @ptr: location to decode from.
 * @result: pointer to string where resulting decoded string is stored
 *
 * result will point to a g_malloc()ed string.
 *
 * Returns: the new ptr to continue decoding
 */
const char *
get_encoded_typedef_or_ref (MonoImage *m, const char *ptr, char **result)
{
	guint32 token;
	
	token = mono_metadata_decode_value (ptr, &ptr);

	*result = get_typedef_or_ref (m, token);

	return ptr;
}

/**
 * get_custom_mod:
 *
 * Decodes a CustomMod (22.2.7)
 *
 * Returns: updated pointer location
 */
const char *
get_custom_mod (MonoImage *m, const char *ptr, char **return_value)
{
	char *s;
	const char *reqd;
	
	*return_value = NULL;
	while ((*ptr == MONO_TYPE_CMOD_OPT) ||
		   (*ptr == MONO_TYPE_CMOD_REQD)) {
		reqd = (*ptr == MONO_TYPE_CMOD_REQD) ? "reqd" : "opt";
		ptr++;
		ptr = get_encoded_typedef_or_ref (m, ptr, &s);

		if (*return_value == NULL)
			*return_value = g_strconcat (reqd, " ", s, NULL);
		else
			*return_value = g_strconcat (*return_value, " ", reqd, " ", s, NULL);
		g_free (s);
	}
	return ptr;
}


static map_t element_type_map [] = {
	{ MONO_TYPE_END        , "end" },
	{ MONO_TYPE_VOID       , "void" },
	{ MONO_TYPE_BOOLEAN    , "bool" },
	{ MONO_TYPE_CHAR       , "char" }, 
	{ MONO_TYPE_I1         , "int8" },
	{ MONO_TYPE_U1         , "unsigned int8" }, 
	{ MONO_TYPE_I2         , "int16" },
	{ MONO_TYPE_U2         , "unsigned int16" },
	{ MONO_TYPE_I4         , "int32" },
	{ MONO_TYPE_U4         , "unsigned int32" },
	{ MONO_TYPE_I8         , "int64" },
	{ MONO_TYPE_U8         , "unsigned int64" },
	{ MONO_TYPE_R4         , "float32" },
	{ MONO_TYPE_R8         , "float64" },
	{ MONO_TYPE_STRING     , "string" },
	{ MONO_TYPE_TYPEDBYREF , "TypedByRef" },
	{ MONO_TYPE_I          , "native int" },
	{ MONO_TYPE_U          , "native unsigned int" },
	{ MONO_TYPE_OBJECT     , "object" },
	{ 0, NULL }
};

static map_t call_conv_type_map [] = {
	{ MONO_CALL_DEFAULT     , "default" },
	{ MONO_CALL_C           , "c" },
	{ MONO_CALL_STDCALL     , "stdcall" },
	{ MONO_CALL_THISCALL    , "thiscall" },
	{ MONO_CALL_FASTCALL    , "fastcall" },
	{ MONO_CALL_VARARG      , "vararg" },
	{ 0, NULL }
};

char*
dis_stringify_token (MonoImage *m, guint32 token)
{
	guint idx = token & 0xffffff;
	switch (token >> 24) {
	case MONO_TABLE_TYPEDEF: return get_typedef (m, idx);
	case MONO_TABLE_TYPEREF: return get_typeref (m, idx);
	case MONO_TABLE_TYPESPEC: return get_typespec (m, idx);
	default:
		 break;
	}
	return g_strdup_printf("0x%08x", token);
}

char*
dis_stringify_array (MonoImage *m, MonoArrayType *array) 
{
	char *type;
	GString *s = g_string_new("");
	int i;
	
	type = dis_stringify_type (m, &array->eklass->byval_arg);
	g_string_append (s, type);
	g_free (type);
	g_string_append_c (s, '[');
	for (i = 0; i < array->rank; ++i) {
		if (i)
			g_string_append_c (s, ',');
		if (i < array->numsizes) {
			if (i < array->numlobounds && array->lobounds[i] != 0)
				g_string_sprintfa (s, "%d..%d", array->lobounds[i], array->sizes[i]);
			else
				g_string_sprintfa (s, "%d", array->sizes[i]);
		}
	}
	g_string_append_c (s, ']');
	type = s->str;
	g_string_free (s, FALSE);
	return type;
}

char*
dis_stringify_modifiers (MonoImage *m, int n, MonoCustomMod *mod)
{
	GString *s = g_string_new("");
	char *result;
	int i;
	for (i = 0; i < n; ++i) {
		char *tok = dis_stringify_token (m, mod[i].token);
		if (i > 0)
			g_string_sprintfa (s, " ");
		g_string_sprintfa (s, "%s (%s)", mod[i].required ? "modopt": "modreq", tok);
		g_free (tok);
	}
	g_string_append_c (s, ' ');
	result = s->str;
	g_string_free (s, FALSE);
	return result;
}

char*
dis_stringify_param (MonoImage *m, MonoType *param) 
{
	char *t;
	char *result;
	const char *out = param->attrs & 2 ? "[out] ": "";
	t = dis_stringify_type (m, param);
	result = g_strconcat (out, t, NULL);
	g_free (t);
	return result;
}

/**
 * get_generic_param
 * @m: metadata context 
 * @table_type: The type of table we are getting generics for (0 for typedef, 1 for method)
 * @row: The row in the table
 *
 * Returns: Allocated stringified generic parameters
 */
char*
get_generic_param (MonoImage *m, int table_type, guint32 row)
{
        MonoTableInfo *t = &m->tables [MONO_TABLE_GENERICPARAM];
        GString *result = g_string_new ("");
        char *retval;
	guint32 cols [MONO_GENERICPARAM_SIZE];
	int i, own_tok, table, idx, found_count;

        g_assert (table_type != MONO_TYPEORMETHOD_TYPE || table_type != MONO_TYPEORMETHOD_METHOD);
        
        found_count = 0;
	for (i = 1; i <= t->rows; i++) {
		mono_metadata_decode_row (t, i-1, cols, MONO_GENERICPARAM_SIZE);
                own_tok = cols [MONO_GENERICPARAM_OWNER];
                table = own_tok & MONO_TYPEORMETHOD_MASK;
                idx = own_tok >> MONO_TYPEORMETHOD_BITS;
                
                if (table != table_type || idx != row)
                        continue;

                if (found_count == 0)
                        g_string_append_printf (result, "<%s",
                                        mono_metadata_string_heap (m, cols [MONO_GENERICPARAM_NAME]));
                else
                        g_string_append_printf (result, ", %s",
                                        mono_metadata_string_heap (m, cols [MONO_GENERICPARAM_NAME]));
                found_count++;
	}

        if (found_count)
                g_string_append_c (result, '>');
        retval = result->str;
        g_string_free (result, FALSE);
        return retval;
}

char*
dis_stringify_method_signature (MonoImage *m, MonoMethodSignature *method, int methoddef_row, gboolean fully_qualified)
{
	guint32 cols [MONO_METHOD_SIZE];
	guint32 pcols [MONO_PARAM_SIZE];
	guint32 param_index = 0;
	const char *name = "";
	int free_method = 0;
	char *retval;
	char *type = NULL;
        char *gen_param;
	GString *result = g_string_new ("");
	int i;
        
	g_assert (method || methoddef_row);

	if (methoddef_row) {
		mono_metadata_decode_row (&m->tables [MONO_TABLE_METHOD], methoddef_row -1, cols, MONO_METHOD_SIZE);
		if (fully_qualified)
			type = get_typedef (m, mono_metadata_typedef_from_method (m, methoddef_row));
		name = mono_metadata_string_heap (m, cols [MONO_METHOD_NAME]);
		param_index = cols [MONO_METHOD_PARAMLIST];
		if (!method) {
			const char *sig = mono_metadata_blob_heap (m, cols [MONO_METHOD_SIGNATURE]);
			mono_metadata_decode_blob_size (sig, &sig);
			method = mono_metadata_parse_method_signature (m, methoddef_row, sig, &sig);
			free_method = 1;
		}      
                gen_param = get_generic_param (m, 1, methoddef_row);
	}
	
	retval = dis_stringify_param (m, method->ret);
	if (method->hasthis)
		g_string_append (result, "instance ");
	g_string_append (result, map (method->call_convention, call_conv_type_map));
	g_string_sprintfa (result, " %s ", retval);
	if (type)
		g_string_sprintfa (result, "%s::", type);
        g_string_append (result, name);
        if (gen_param) {
                g_string_append (result, gen_param);
                g_free (gen_param);
        }
	g_string_append (result, " (");
	g_free (retval);
	for (i = 0; i < method->param_count; ++i) {
		if (param_index && param_index <= m->tables [MONO_TABLE_PARAM].rows) {
			mono_metadata_decode_row (&m->tables [MONO_TABLE_PARAM], param_index - 1, pcols, MONO_PARAM_SIZE);
			name = mono_metadata_string_heap (m, pcols [MONO_PARAM_NAME]);
			method->params [i]->attrs = pcols [MONO_PARAM_FLAGS];
			param_index++;
		} else {
			name = "";
		}
		if (i)
			g_string_append (result, ", ");
		retval = dis_stringify_param (m, method->params [i]);
		g_string_sprintfa (result, "%s '%s'", retval, name);
		g_free (retval);
	}
	g_string_append (result, ") ");

	if (free_method)
		mono_metadata_free_method_signature (method);
	retval = result->str;
	g_string_free (result, FALSE);

	return retval;
}

static char *
dis_stringify_object_with_class (MonoImage *m, MonoClass *c)
{
	/* FIXME: handle MONO_TYPE_OBJECT ... */
	const char *otype = c->byval_arg.type == MONO_TYPE_CLASS? "class" : "valuetype";
	char *assemblyref = NULL, *result;
	if (m != c->image) {
		if (c->image->assembly_name) {
			/* we cheat */
			if (substitute_with_mscorlib_p && !strcmp ("corlib", c->image->assembly_name))
				assemblyref = g_strdup_printf ("[%s]", "mscorlib");
			else
				assemblyref = g_strdup_printf ("[%s]", c->image->assembly->aname.name);
		} else {
			assemblyref = g_strdup_printf ("[.module %s]", c->image->module_name);
		}
	}
	result = g_strdup_printf ("%s %s%s%s%s", otype, assemblyref?assemblyref:"", c->name_space, 
				*c->name_space?".":"", c->name);
	g_free (assemblyref);
	return result;
}

static char *
dis_stringify_object (MonoImage *m, MonoType *type)
{
	MonoClass *c = mono_class_from_mono_type (type);
	return dis_stringify_object_with_class (m, c);
}

char*
dis_stringify_type (MonoImage *m, MonoType *type)
{
	const char *pinned = "", *byref = "";
	char *bare = NULL, *mods = NULL;
	char *result;

	if (type->num_mods)
		mods = dis_stringify_modifiers (m, type->num_mods, type->modifiers);

	switch (type->type){
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
		bare = g_strdup (map (type->type, element_type_map));
		break;
		
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_CLASS:
		bare = dis_stringify_object (m, type);
		break;
		
	case MONO_TYPE_FNPTR:
		bare = dis_stringify_method_signature (m, type->data.method, 0, FALSE);
		break;
	case MONO_TYPE_PTR: {
		char *child_type;
		child_type = dis_stringify_type (m, type->data.type);
		
		bare = g_strdup_printf ("%s*", child_type);
		g_free (child_type);
		break;
	}
	case MONO_TYPE_SZARRAY: {
		char *child_type;
		child_type = dis_stringify_type (m, &type->data.klass->byval_arg);
		
		bare = g_strdup_printf ("%s[]", child_type);
		g_free (child_type);
		break;
	}
	case MONO_TYPE_ARRAY:
		bare = dis_stringify_array (m, type->data.array);
		break;
	case MONO_TYPE_VOID:
		bare = g_strdup ("void");
		break;
	case MONO_TYPE_MVAR:
		bare = g_strdup_printf ("!!%d", type->data.generic_param->num);
		break;
	case MONO_TYPE_VAR:
		bare = g_strdup_printf ("!%d", type->data.generic_param->num);
		break;
	case MONO_TYPE_GENERICINST: {
		GString *str = g_string_new ("");
		int i;
		char *generic_type = dis_stringify_type (m, type->data.generic_inst->generic_type);

		for (i = 0; i < type->data.generic_inst->type_argc; i++){
			char *t = dis_stringify_type (m, type->data.generic_inst->type_argv [i]);

			g_string_append (str, t);
			if (i+1 != type->data.generic_inst->type_argc)
				g_string_append (str, ", ");
			g_free (t);
		}
		bare = g_strdup_printf ("%s<%s>", generic_type, str->str);
		g_string_free (str, TRUE);
		break;
	}
		
	default:
		g_error ("Do not know how to stringify type 0x%x", type->type);
	}
	
	if (type->pinned)
		pinned = " pinned";

	if (type->byref)
		byref = "&";
		
	result = g_strconcat (mods ? mods : "", bare, byref, pinned, NULL);

	g_free (bare);

	return result;
}

/**
 * get_type:
 * @m: metadata context 
 * @ptr: location to decode from.
 * @result: pointer to string where resulting decoded string is stored
 *
 * This routine returs in @result the stringified type pointed by @ptr.
 * (22.2.12)
 *
 * Returns: the new ptr to continue decoding
 */
const char *
get_type (MonoImage *m, const char *ptr, char **result)
{
	MonoType *type = mono_metadata_parse_type (m, MONO_PARSE_TYPE, 0, ptr, &ptr);
	*result = dis_stringify_type (m, type);
	mono_metadata_free_type (type);
	return ptr;
}

/**
 * 
 * Returns a stringified representation of a FieldSig (22.2.4)
 */
char *
get_field_signature (MonoImage *m, guint32 blob_signature)
{
	char *allocated_modifier_string, *allocated_type_string;
	const char *ptr = mono_metadata_blob_heap (m, blob_signature);
	const char *base;
	char *res;
	int len;
	
	len = mono_metadata_decode_value (ptr, &ptr);
	base = ptr;
	/* FIELD is 0x06 */
	g_assert (*ptr == 0x06);
/*	hex_dump (ptr, 0, len); */
	ptr++; len--;
	
	ptr = get_custom_mod (m, ptr, &allocated_modifier_string);
	ptr = get_type (m, ptr, &allocated_type_string);

	res = g_strdup_printf (
		"%s %s",
		allocated_modifier_string ? allocated_modifier_string : "",
		allocated_type_string);
	
	if (allocated_modifier_string)
		g_free (allocated_modifier_string);
	if (allocated_type_string)
		g_free (allocated_type_string);
	
	return res;
}

MonoTypeEnum
get_field_literal_type (MonoImage *m, guint32 blob_signature)
{
	const char *ptr = mono_metadata_blob_heap (m, blob_signature);
	int len;
	char *allocated_modifier_string;
	
	len = mono_metadata_decode_value (ptr, &ptr);

	/* FIELD is 0x06 */
	g_assert (*ptr == 0x06);
	ptr++; len--;
	
	ptr = get_custom_mod (m, ptr, &allocated_modifier_string);
	if (allocated_modifier_string)
		g_free (allocated_modifier_string);

	return (MonoTypeEnum) *ptr;
	
}

/**
 * decode_literal:
 * @m: metadata context
 * @token: token to decode
 *
 * decodes the literal indexed by @token.
 */
char *
decode_literal (MonoImage *m, guint32 token)
{
	return g_strdup ("LITERAL_VALUE");
}

/**
 * get_ret_type:
 * @m: metadata context 
 * @ptr: location to decode from.
 * @result: pointer to string where resulting decoded string is stored
 *
 * This routine returns in @result the stringified RetType (22.2.11)
 *
 * Returns: the new ptr to continue decoding.
 */
const char *
get_ret_type (MonoImage *m, const char *ptr, char **ret_type)
{
	GString *str = g_string_new ("");
	char *mod = NULL;
	char *allocated_type_string;
	
	ptr = get_custom_mod (m, ptr, &mod);
	if (mod){
		g_string_append (str, mod);
		g_string_append_c (str, ' ');
		g_free (mod);
	}

	if (*ptr == MONO_TYPE_TYPEDBYREF){
		/* TODO: what does `typedbyref' mean? */
		g_string_append (str, "/* FIXME: What does this mean? */ typedbyref ");
		ptr++;
	} else if (*ptr == MONO_TYPE_VOID){
		 g_string_append (str, "void");
		 ptr++;
	} else {
                if (*ptr == MONO_TYPE_BYREF){
			g_string_append (str, "[out] ");
			ptr++;
		}

		ptr = get_type (m, ptr, &allocated_type_string);
		g_string_append (str, allocated_type_string);
		g_free (allocated_type_string);
	}

	*ret_type = str->str;
	g_string_free (str, FALSE);

	return ptr;
}

/**
 * get_param:
 * @m: metadata context 
 * @ptr: location to decode from.
 * @result: pointer to string where resulting decoded string is stored
 *
 * This routine returns in @result the stringified Param (22.2.10)
 *
 * Returns: the new ptr to continue decoding.
 */
const char *
get_param (MonoImage *m, const char *ptr, char **retval)
{
	GString *str = g_string_new ("");
	char *allocated_mod_string, *allocated_type_string;
	
	ptr = get_custom_mod (m, ptr, &allocated_mod_string);
	if (allocated_mod_string){
		g_string_append (str, allocated_mod_string);
		g_string_append_c (str, ' ');
		g_free (allocated_mod_string);
	}
	
	if (*ptr == MONO_TYPE_TYPEDBYREF){
		g_string_append (str, " typedbyref ");
		ptr++;
	} else {
		 if (*ptr == MONO_TYPE_BYREF){
			g_string_append (str, "[out] ");
			ptr++;
		}
		ptr = get_type (m, ptr, &allocated_type_string);
		g_string_append (str, allocated_type_string);
		g_free (allocated_type_string);
	}

	*retval = str->str;
	g_string_free (str, FALSE);
	return ptr;
}

static map_t param_map [] = {
	{ PARAM_ATTRIBUTE_IN,                "[in] " },
	{ PARAM_ATTRIBUTE_OUT,               "[out] " },
	{ PARAM_ATTRIBUTE_OPTIONAL,          "optional " },
	{ PARAM_ATTRIBUTE_HAS_DEFAULT,       "hasdefault " },
	{ PARAM_ATTRIBUTE_HAS_FIELD_MARSHAL, "fieldmarshal " },
	{ 0, NULL }
};

char *
param_flags (guint32 f)
{
	return g_strdup (flags (f, param_map));
}

static map_t field_access_map [] = {
	{ FIELD_ATTRIBUTE_COMPILER_CONTROLLED, "compilercontrolled " },
	{ FIELD_ATTRIBUTE_PRIVATE,             "private " },
	{ FIELD_ATTRIBUTE_FAM_AND_ASSEM,       "famandassem " },
	{ FIELD_ATTRIBUTE_ASSEMBLY,            "assembly " },
	{ FIELD_ATTRIBUTE_FAMILY,              "family " },
	{ FIELD_ATTRIBUTE_FAM_OR_ASSEM,        "famorassem " },
	{ FIELD_ATTRIBUTE_PUBLIC,              "public " },
	{ 0, NULL }
};

static map_t field_flags_map [] = {
	{ FIELD_ATTRIBUTE_STATIC,              "static " },
	{ FIELD_ATTRIBUTE_INIT_ONLY,           "initonly " },
	{ FIELD_ATTRIBUTE_LITERAL,             "literal " },
	{ FIELD_ATTRIBUTE_NOT_SERIALIZED,      "notserialized " },
	{ FIELD_ATTRIBUTE_SPECIAL_NAME,        "specialname " },
	{ FIELD_ATTRIBUTE_PINVOKE_IMPL,        "FIXME:pinvokeimpl " },
	{ FIELD_ATTRIBUTE_RT_SPECIAL_NAME,        "rtspecialname " },
	{ FIELD_ATTRIBUTE_HAS_FIELD_MARSHAL,        "hasfieldmarshal " },
	{ FIELD_ATTRIBUTE_HAS_DEFAULT,        "hasdefault " },
	{ FIELD_ATTRIBUTE_HAS_FIELD_RVA,        "hasfieldrva " },
	{ 0, NULL }
};

/**
 * field_flags:
 *
 * Returns a stringified version of a Field's flags
 */
char *
field_flags (guint32 f)
{
	char buffer [1024];
	int access = f & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
	
	buffer [0] = 0;

	strcat (buffer, map (access, field_access_map));
	strcat (buffer, flags (f, field_flags_map));
	return g_strdup (buffer);
}

/**
 * Returns a stringifed representation of a MethodRefSig (22.2.2)
 */
char *
get_methodref_signature (MonoImage *m, guint32 blob_signature, const char *fancy_name)
{
	GString *res = g_string_new ("");
	const char *ptr = mono_metadata_blob_heap (m, blob_signature);
	char *allocated_ret_type, *s;
	gboolean seen_vararg = 0;
	int param_count, signature_len;
	int i, gen_count = 0;
	int cconv;
	
	signature_len = mono_metadata_decode_value (ptr, &ptr);

	if (*ptr & 0x20){
		if (*ptr & 0x40)
			g_string_append (res, "explicit-this ");
		else
			g_string_append (res, "instance "); /* has-this */
	}

	if (*ptr & 0x05)
		seen_vararg = 1;
	if (*ptr & 0x10)
		gen_count = 1;
	cconv = *ptr & 0x0f;
	ptr++;
	if (gen_count)
		gen_count = mono_metadata_decode_value (ptr, &ptr);
	param_count = mono_metadata_decode_value (ptr, &ptr);
	if (cconv != 0xa) {
		ptr = get_ret_type (m, ptr, &allocated_ret_type);
		g_string_append (res, allocated_ret_type);
		g_free (allocated_ret_type);
	}

	if (fancy_name){
		g_string_append_c (res, ' ');
		g_string_append (res, fancy_name);
	}
	
	g_string_append (res, "(");
	
	/*
	 * param_count describes parameters *before* and *after*
	 * the vararg sentinel
	 */
	for (i = 0; i < param_count; i++){
		char *param = NULL;
		
		/*
		 * If ptr is a SENTINEL
		 */
		if (*ptr == 0x41){
			g_string_append (res, " varargs ");
			continue;
		}

		ptr = get_param (m, ptr, &param);
		g_string_append (res, param);
		if (i+1 != param_count)
			g_string_append (res, ", ");
		g_free (param);
	}
	g_string_append (res, ")");
	
	/*
	 * cleanup and return
	 */
	s = res->str;
	g_string_free (res, FALSE);
	return s;
}

/**
 * Returns a stringifed representation of a field ref
 */
char *
get_fieldref_signature (MonoImage *m, int idx)
{
        guint32 cols [MONO_MEMBERREF_SIZE];
        char *sig;
        char *full_sig;

        mono_metadata_decode_row (&m->tables [MONO_TABLE_MEMBERREF],
                        idx - 1, cols, MONO_MEMBERREF_SIZE);

        sig = get_field_signature (m, cols [MONO_FIELD_SIGNATURE]);
        full_sig = g_strdup_printf ("%s %s::%s", sig,
                        get_memberref_parent (m, cols [MONO_MEMBERREF_CLASS]),
                        mono_metadata_string_heap (m, cols [MONO_MEMBERREF_NAME]));
        g_free (sig);
        
        return full_sig;
}

/**
 * get_field:
 * @m: metadata context
 * @token: a FIELD_DEF token
 *
 * This routine has to locate the TypeDef that "owns" this Field.
 * Since there is no backpointer in the Field table, we have to scan
 * the TypeDef table and locate the actual "owner" of the field
 */
char *
get_field (MonoImage *m, guint32 token)
{
	int idx = mono_metadata_token_index (token);
	guint32 cols [MONO_FIELD_SIZE];
	char *sig, *res, *type;
	guint32 type_idx;

	/*
	 * We can get here also with a MenberRef token (for a field
	 * defined in another module/assembly, just like in get_method ()
	 */
	if (mono_metadata_token_code (token) == MONO_TOKEN_MEMBER_REF) {
                return get_fieldref_signature (m, idx);
	}
	g_assert (mono_metadata_token_code (token) == MONO_TOKEN_FIELD_DEF);

	mono_metadata_decode_row (&m->tables [MONO_TABLE_FIELD], idx - 1, cols, MONO_FIELD_SIZE);
	sig = get_field_signature (m, cols [MONO_FIELD_SIGNATURE]);

	/*
	 * To locate the actual "container" for this field, we have to scan
	 * the TypeDef table.  LAME!
	 */
	type_idx = mono_metadata_typedef_from_field (m, idx);

	type = get_typedef (m, type_idx);
	res = g_strdup_printf ("%s %s::%s",
			       sig, type,
			       mono_metadata_string_heap (m, cols [MONO_FIELD_NAME]));
	g_free (type);
	g_free (sig);

	return res;
}

static char *
get_memberref_parent (MonoImage *m, guint32 mrp_token)
{
	/*
	 * mrp_index is a MemberRefParent coded index
	 */
	guint32 table = mrp_token & 7;
	guint32 idx = mrp_token >> 3;

	switch (table){
	case 0: /* TypeDef */
		return get_typedef (m, idx);
		
	case 1: /* TypeRef */
		return get_typeref (m, idx);
		
	case 2: /* ModuleRef */
		return g_strdup_printf ("TODO:MemberRefParent-ModuleRef");
		
	case 3: /* MethodDef */
		return g_strdup ("TODO:MethodDef");
		
	case 4: /* TypeSpec */
		return get_typespec (m, idx);
	}
	g_assert_not_reached ();
	return NULL;
}

/**
 * get_method:
 * @m: metadata context
 * @token: a METHOD_DEF or MEMBER_REF token
 *
 * This routine has to locate the TypeDef that "owns" this Field.
 * Since there is no backpointer in the Field table, we have to scan
 * the TypeDef table and locate the actual "owner" of the field
 */
char *
get_method (MonoImage *m, guint32 token)
{
	int idx = mono_metadata_token_index (token);
	guint32 member_cols [MONO_MEMBERREF_SIZE], method_cols [MONO_METHOD_SIZE];
	char *sig;
	const char *name;

	MonoMethod *mh;

	mh = mono_get_method (m, token, NULL);
	if (mh) {
		sig = dis_stringify_object_with_class (m, mh->klass);
		name = g_strdup_printf ("%s::%s", sig, mh->name);
		g_free (sig);
	} else
		name = NULL;

	switch (mono_metadata_token_code (token)){
	case MONO_TOKEN_METHOD_DEF:
		mono_metadata_decode_row (&m->tables [MONO_TABLE_METHOD], 
					  idx - 1, method_cols, MONO_METHOD_SIZE);

		sig = get_methodref_signature (m, method_cols [MONO_METHOD_SIGNATURE], name);
		return sig;
		
	case MONO_TOKEN_MEMBER_REF: {
		mono_metadata_decode_row (&m->tables [MONO_TABLE_MEMBERREF],
					  idx - 1, member_cols, MONO_MEMBERREF_SIZE);
		if (!name)
			name = g_strdup_printf ("%s::%s",
					get_memberref_parent (m, member_cols [MONO_MEMBERREF_CLASS]),
					mono_metadata_string_heap (m, member_cols [MONO_MEMBERREF_NAME]));
		sig = get_methodref_signature (
			m, member_cols [MONO_MEMBERREF_SIGNATURE], name);
		return sig;
	}
	case MONO_TOKEN_METHOD_SPEC: {
		mono_metadata_decode_row (&m->tables [MONO_TABLE_METHODSPEC], idx - 1, member_cols, MONO_METHODSPEC_SIZE);
		token = member_cols [MONO_METHODSPEC_METHOD];
		if ((token & METHODDEFORREF_MASK) == METHODDEFORREF_METHODDEF)
			token = MONO_TOKEN_METHOD_DEF | (token >> METHODDEFORREF_BITS);
		else
			token = MONO_TOKEN_MEMBER_REF | (token >> METHODDEFORREF_BITS);
		return get_method (m, token);

	}

	default:
		g_assert_not_reached ();
	}
	g_assert_not_reached ();
	return NULL;
}

/**
 * get_methoddef
 * @m: metadata context
 * @idx: index into the method table
 *
 * Returns: A stringified version of the method signature.
 */
char *
get_methoddef (MonoImage *m, guint32 idx)
{
        guint32 cols [MONO_METHOD_SIZE];
	char *sig;
	const char *name;

	MonoMethod *mh;

	mh = mono_get_method (m, MONO_TOKEN_METHOD_DEF | idx, NULL);
	if (mh) {
		sig = dis_stringify_object_with_class (m, mh->klass);
		name = g_strdup_printf ("%s::%s", sig, mh->name);
		g_free (sig);
	} else
		name = NULL;
        mono_metadata_decode_row (&m->tables [MONO_TABLE_METHOD], 
                        idx - 1, cols, MONO_METHOD_SIZE);
        sig = get_methodref_signature (m, cols [MONO_METHOD_SIGNATURE], name);
        
        return sig;
}

/**
 * get_constant:
 * @m: metadata context
 * @blob_index: index into the blob where the constant is stored
 *
 * Returns: An allocated value representing a stringified version of the
 * constant.
 */
char *
get_constant (MonoImage *m, MonoTypeEnum t, guint32 blob_index)
{
	const char *ptr = mono_metadata_blob_heap (m, blob_index);
	int len;
	
	len = mono_metadata_decode_value (ptr, &ptr);
	
	switch (t){
	case MONO_TYPE_BOOLEAN:
		return g_strdup_printf ("%s", *ptr ? "true" : "false");
		
	case MONO_TYPE_CHAR:
		return g_strdup_printf ("%c", *ptr); /* FIXME: unicode char */
		
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
		return g_strdup_printf ("int8(0x%02x)", (int) (*ptr));
		break;
		
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
		return g_strdup_printf ("int16(0x%08x)", (int) read16 (ptr));
		
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
		return g_strdup_printf ("int32(%d)", read32 (ptr));
		
	case MONO_TYPE_I8: {
		guint32 low, high;
		low = read32 (ptr);
		high = read32 (ptr + 4);
		return g_strdup_printf ("0x%08x%08x", high, low);
	}
	case MONO_TYPE_U8: {
		guint32 low, high;
		low = read32 (ptr);
		high = read32 (ptr + 4);
		return g_strdup_printf ("0x%08x%08x", high, low);
	}
	case MONO_TYPE_R4: {
		float r;
		readr4 (ptr, &r);
		return g_strdup_printf ("%g", (double) r);
	}
	case MONO_TYPE_R8: {
		double r;
		readr8 (ptr, &r);
		return g_strdup_printf ("%g", r);
	}
	case MONO_TYPE_STRING: {
		int i, j, e;
		char *res;
		e = len = 0;
		for (i = 0; !ptr [i+1]; i += 2){
			len++;
			switch (ptr [i]) {
			case '"':
			case '\\':
			case '\n': /* add more */
				e++;
			}
		}
		res = g_malloc (len + e + 3);
		j = 1;
		res [0] = '"';

		for (i = 0; i < len; i += 2){
			switch(ptr[i]) {
			case '"': 
				res[j++] = '\\';
				res[j++] = '"';
			case '\\': 
				res[j++] = '\\';
				res[j++] = '\\';
			case '\n':
				res[j++] = '\\';
				res[j++] = 'n';
				break;
			default:
				res[j++] = isprint (ptr [i]) ? ptr [i] : '.';
				break;
			}
		}
		res[j++] = '"';
		res[j] = 0;
		return res;
	}
		
	case MONO_TYPE_CLASS:
		return g_strdup ("CLASS CONSTANT.  MUST BE ZERO");
		
	default:
		g_error ("Unknown MONO_TYPE (%d) on constant at Blob index (0x%08x)\n",
			 (int) *ptr, blob_index);
		return g_strdup_printf ("Unknown");
	}

}

/**
 * get_token:
 * @m: metadata context
 * @token: token that we want to decode.
 *
 * Returns: An allocated value representing a stringified version of the
 * constant.
 */
char *
get_token (MonoImage *m, guint32 token)
{
	guint32 idx = mono_metadata_token_index (token);

	switch (mono_metadata_token_code (token)){
	case MONO_TOKEN_FIELD_DEF:
		return (get_field (m, token));
	case MONO_TOKEN_TYPE_DEF:
		return get_typedef (m, idx);
	case MONO_TOKEN_TYPE_REF:
		return get_typeref (m, idx);
	case MONO_TOKEN_TYPE_SPEC:
		return get_typespec (m, idx);
	default:		
		g_error ("Do not know how to decode tokens of type 0x%08x", token);
	}

	g_assert_not_reached ();
	return g_strdup ("ERROR");
}

/**
 * get_token_type:
 * @m: metadata context
 * @token: the token can belong to any of the following tables:
 * MONO_TOKEN_TYPE_REF, MONO_TOKEN_TYPE_DEF, MONO_TOKEN_TYPE_SPEC
 *
 * Returns: a stringified version of the MethodDef or MethodRef or TypeSpecn
 * at (token & 0xffffff) 
 */
char *
get_token_type (MonoImage *m, guint32 token)
{
	char *temp = NULL, *s;
	int idx;

	idx = mono_metadata_token_index (token);
	
	switch (mono_metadata_token_code (token)){
	case MONO_TOKEN_TYPE_DEF:
		temp = get_typedef (m, idx);
		s = g_strdup_printf ("%s", temp);
		break;
		
	case MONO_TOKEN_TYPE_REF: 
		temp = get_typeref (m, idx);
		s = g_strdup_printf ("%s", temp);
		break;
		
	case MONO_TOKEN_TYPE_SPEC:
		s = get_typespec (m, idx);
		break;

	default:
		g_error ("Unhandled encoding for token 0x%08x", token);

	}
	
	if (temp)
		g_free (temp);

	return s;
}

char *
get_guid (MonoImage *m, guint32 guid_index)
{
	const unsigned char *guid;
	char *result;

	guid = mono_metadata_guid_heap (m, guid_index);

	result = g_strdup_printf ("{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}", 
			guid [3], guid [2], guid [1], guid [0], guid [5], guid [4], guid [7], guid [6],
			guid [8], guid [9], guid [10], guid [11], guid [12], guid [13], guid [14], guid [15]);
	return result;
}

GList *
dis_get_custom_attrs (MonoImage *m, guint32 token)
{
	GList *list = NULL;
	guint32 idx, i, len, mtoken;
	guint32 cols [MONO_CUSTOM_ATTR_SIZE];
	MonoTableInfo *ca;
	char *method;
	GString *attr;
	const char *val;

	idx = mono_metadata_token_index (token);
	idx <<= CUSTOM_ATTR_BITS;
	
	switch (mono_metadata_token_table (token)) {
	case MONO_TABLE_TYPEDEF:
		idx |= CUSTOM_ATTR_TYPEDEF;
		break;
	case MONO_TABLE_ASSEMBLY:
		idx |= CUSTOM_ATTR_ASSEMBLY;
		break;
	case MONO_TABLE_PROPERTY:
		idx |= CUSTOM_ATTR_PROPERTY;
		break;
	case MONO_TABLE_EVENT:
		idx |= CUSTOM_ATTR_EVENT;
		break;
	case MONO_TABLE_FIELD:
		idx |= CUSTOM_ATTR_FIELDDEF;
		break;
	case MONO_TABLE_METHOD:
		idx |= CUSTOM_ATTR_METHODDEF;
		break;
	case MONO_TABLE_PARAM:
		idx |= CUSTOM_ATTR_PARAMDEF;
		break;
	default:
		g_print ("Missing custom attr get support for token 0x%08x\n", token);
		return NULL;
	}

	ca = &m->tables [MONO_TABLE_CUSTOMATTRIBUTE];
	/* the table is not sorted */
	for (i = 0; i < ca->rows; ++i) {
		char *dump;
		mono_metadata_decode_row (ca, i, cols, MONO_CUSTOM_ATTR_SIZE);
		if (cols [MONO_CUSTOM_ATTR_PARENT] != idx)
			continue;
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
		method = get_method (m, mtoken);
		val = mono_metadata_blob_heap (m, cols [MONO_CUSTOM_ATTR_VALUE]);
		len = mono_metadata_decode_value (val, &val);
		attr = g_string_new (".custom ");
		dump = data_dump (val, len, "\t\t");
		g_string_sprintfa (attr, "%s = %s", method, dump);
		g_free (dump);
		list = g_list_append (list, attr->str);
		g_string_free (attr, FALSE);
		g_free (method);
	}
	return list;
}

char*
get_marshal_info (MonoImage *m, const char *blob) {
	int len, size = 0;

	len = mono_metadata_decode_blob_size (blob, &blob);

	switch (*blob) {
	case MONO_NATIVE_BOOLEAN:
		return g_strdup ("bool");
	case MONO_NATIVE_I1:
		return g_strdup ("int8");
	case MONO_NATIVE_U1:
		return g_strdup ("unsigned int8");
	case MONO_NATIVE_I2:
		return g_strdup ("int16");
	case MONO_NATIVE_U2:
		return g_strdup ("unsigned int16");
	case MONO_NATIVE_I4:
		return g_strdup ("int32");
	case MONO_NATIVE_U4:
		return g_strdup ("unsigned int32");
	case MONO_NATIVE_I8:
		return g_strdup ("int64");
	case MONO_NATIVE_U8:
		return g_strdup ("unsigned int64");
	case MONO_NATIVE_R4:
		return g_strdup ("float32");
	case MONO_NATIVE_R8:
		return g_strdup ("float64");
	case MONO_NATIVE_CURRENCY:
		return g_strdup ("currency");
	case MONO_NATIVE_BSTR:
		return g_strdup ("bstr");
	case MONO_NATIVE_LPSTR:
		return g_strdup ("lpstr");
	case MONO_NATIVE_LPWSTR:
		return g_strdup ("lpwstr");
	case MONO_NATIVE_LPTSTR:
		return g_strdup ("lptstr");
	case MONO_NATIVE_BYVALTSTR:
		size = mono_metadata_decode_value (blob + 1, &blob);
		return g_strdup_printf ("fixed sysstring [%d]", size);
	case MONO_NATIVE_IUNKNOWN:
		return g_strdup ("iunknown");
	case MONO_NATIVE_IDISPATCH:
		return g_strdup ("idispatch");
	case MONO_NATIVE_STRUCT:
		return g_strdup ("struct");
	case MONO_NATIVE_INTERFACE:
		return g_strdup ("interface");
	case MONO_NATIVE_SAFEARRAY:
		return g_strdup ("safearray");
	case MONO_NATIVE_BYVALARRAY:
		size = mono_metadata_decode_value (blob + 1, &blob);
		return g_strdup_printf ("fixed array [%d]", size);
	case MONO_NATIVE_INT:
		return g_strdup ("int");
	case MONO_NATIVE_UINT:
		return g_strdup ("unsigned int");
	case MONO_NATIVE_VBBYREFSTR:
		return g_strdup ("vbbyrefstr");
	case MONO_NATIVE_ANSIBSTR:
		return g_strdup ("ansi bstr");
	case MONO_NATIVE_TBSTR:
		return g_strdup ("tbstr");
	case MONO_NATIVE_VARIANTBOOL:
		return g_strdup ("variant bool");
	case MONO_NATIVE_FUNC:
		return g_strdup ("method");
	case MONO_NATIVE_ASANY:
		return g_strdup ("as any");
	case MONO_NATIVE_LPARRAY:
		return g_strdup ("[]");
	case MONO_NATIVE_LPSTRUCT:
		return g_strdup ("lpstruct");
	case MONO_NATIVE_CUSTOM:
		return g_strdup ("custom");
	case MONO_NATIVE_ERROR:
		return g_strdup ("error");
	default:
		return g_strdup ("unknown");
	}
}

