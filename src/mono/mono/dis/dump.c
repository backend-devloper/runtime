/*
 * dump.c: Dumping routines for the disassembler.
 *
 * Author:
 *   Miguel de Icaza (miguel@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */
#include <config.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "meta.h"
#include "util.h"
#include "dump.h"
#include "get.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/class.h"

void
dump_table_assembly (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_ASSEMBLY];
	guint32 cols [MONO_ASSEMBLY_SIZE];
	const char *ptr;
	int len;

	mono_metadata_decode_row (t, 0, cols, MONO_ASSEMBLY_SIZE);
	fprintf (output, "Assembly Table\n");

	fprintf (output, "Name:          %s\n", mono_metadata_string_heap (m, cols [MONO_ASSEMBLY_NAME]));
	fprintf (output, "Hash Algoritm: 0x%08x\n", cols [MONO_ASSEMBLY_HASH_ALG]);
	fprintf (output, "Version:       %d.%d.%d.%d\n", cols [MONO_ASSEMBLY_MAJOR_VERSION], 
					cols [MONO_ASSEMBLY_MINOR_VERSION], 
					cols [MONO_ASSEMBLY_BUILD_NUMBER], 
					cols [MONO_ASSEMBLY_REV_NUMBER]);
	fprintf (output, "Flags:         0x%08x\n", cols [MONO_ASSEMBLY_FLAGS]);
	fprintf (output, "PublicKey:     BlobPtr (0x%08x)\n", cols [MONO_ASSEMBLY_PUBLIC_KEY]);

	ptr = mono_metadata_blob_heap (m, cols [MONO_ASSEMBLY_PUBLIC_KEY]);
	len = mono_metadata_decode_value (ptr, &ptr);
	if (len > 0){
		fprintf (output, "\tDump:");
		hex_dump (ptr, 0, len);
		fprintf (output, "\n");
	} else
		fprintf (output, "\tZero sized public key\n");
	
	fprintf (output, "Culture:       %s\n", mono_metadata_string_heap (m, cols [MONO_ASSEMBLY_CULTURE]));
	fprintf (output, "\n");
}

void
dump_table_typeref (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_TYPEREF];
	int i;

	fprintf (output, "Typeref Table\n");
	
	for (i = 1; i <= t->rows; i++){
		char *s = get_typeref (m, i);
		
		fprintf (output, "%d: %s\n", i, s);
		g_free (s);
	}
	fprintf (output, "\n");
}

void
dump_table_typedef (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_TYPEDEF];
	int i;

	fprintf (output, "Typedef Table\n");
	
	for (i = 1; i <= t->rows; i++){
		char *s = get_typedef (m, i);
		guint32 cols [MONO_TYPEDEF_SIZE];

		mono_metadata_decode_row (&m->tables [MONO_TABLE_TYPEDEF], i - 1, cols, MONO_TYPEDEF_SIZE);

		fprintf (output, "%d: %s (flist=%d, mlist=%d, flags=0x%x, extends=0x%x)\n", i, s, 
					cols [MONO_TYPEDEF_FIELD_LIST], cols [MONO_TYPEDEF_METHOD_LIST],
					cols [MONO_TYPEDEF_FLAGS], cols [MONO_TYPEDEF_EXTENDS]);
		g_free (s);
	}
	fprintf (output, "\n");
}

void
dump_table_assemblyref (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_ASSEMBLYREF];
	int i;

	fprintf (output, "AssemblyRef Table\n");
	
	for (i = 0; i < t->rows; i++){
		const char *ptr;
		int len;
		guint32 cols [MONO_ASSEMBLYREF_SIZE];

		mono_metadata_decode_row (t, i, cols, MONO_ASSEMBLYREF_SIZE);
		fprintf (output, "%d: Version=%d.%d.%d.%d\n\tName=%s\n", i + 1,
			 cols [MONO_ASSEMBLYREF_MAJOR_VERSION], 
			 cols [MONO_ASSEMBLYREF_MINOR_VERSION], 
			 cols [MONO_ASSEMBLYREF_BUILD_NUMBER], 
			 cols [MONO_ASSEMBLYREF_REV_NUMBER],
			 mono_metadata_string_heap (m, cols [MONO_ASSEMBLYREF_NAME]));
		ptr = mono_metadata_blob_heap (m, cols [MONO_ASSEMBLYREF_PUBLIC_KEY]);
		len = mono_metadata_decode_value (ptr, &ptr);
		if (len > 0){
			fprintf (output, "\tPublic Key:");
			hex_dump (ptr, 0, len);
			fprintf (output, "\n");
		} else
			fprintf (output, "\tZero sized public key\n");
		
	}
	fprintf (output, "\n");
}

void
dump_table_param (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_PARAM];
	int i;

	fprintf (output, "Param Table\n");
	
	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_PARAM_SIZE];

		mono_metadata_decode_row (t, i, cols, CSIZE (cols));
		fprintf (output, "%d: 0x%04x %d %s\n",
			 i + 1,
			 cols [MONO_PARAM_FLAGS], cols [MONO_PARAM_SEQUENCE], 
			 mono_metadata_string_heap (m, cols [MONO_PARAM_NAME]));
	}
	fprintf (output, "\n");
}

void
dump_table_field (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_FIELD];
	MonoTableInfo *td = &m->tables [MONO_TABLE_TYPEDEF];
	MonoTableInfo *fl = &m->tables [MONO_TABLE_FIELDLAYOUT];
	MonoTableInfo *rva = &m->tables [MONO_TABLE_FIELDRVA];
	int i, current_type, offset_row, rva_row;
	guint32 first_m, last_m;

	fprintf (output, "Field Table (1..%d)\n", t->rows);
	
	rva_row = offset_row = current_type = 1;
	last_m = first_m = 1;
	for (i = 1; i <= t->rows; i++){
		guint32 cols [MONO_FIELD_SIZE];
		char *sig, *flags;

		/*
		 * Find the next type.
		 */
		while (current_type <= td->rows && i >= (last_m = mono_metadata_decode_row_col (td, current_type - 1, MONO_TYPEDEF_FIELD_LIST))) {
			current_type++;
		}
		if (i == first_m) {
			fprintf (output, "########## %s.%s\n",
				mono_metadata_string_heap (m, mono_metadata_decode_row_col (td, current_type - 2, MONO_TYPEDEF_NAMESPACE)),
				mono_metadata_string_heap (m, mono_metadata_decode_row_col (td, current_type - 2, MONO_TYPEDEF_NAME)));
			first_m = last_m;
		}
		mono_metadata_decode_row (t, i - 1, cols, MONO_FIELD_SIZE);
		sig = get_field_signature (m, cols [MONO_FIELD_SIGNATURE]);
		flags = field_flags (cols [MONO_FIELD_FLAGS]);
		fprintf (output, "%d: %s %s: %s\n",
			 i,
			 sig,
			 mono_metadata_string_heap (m, cols [MONO_FIELD_NAME]),
			 flags);
		g_free (sig);
		g_free (flags);
		if (offset_row <= fl->rows && (mono_metadata_decode_row_col (fl, offset_row - 1, MONO_FIELD_LAYOUT_FIELD) == i)) {
			fprintf (output, "\texplicit offset: %d\n", mono_metadata_decode_row_col (fl, offset_row - 1, MONO_FIELD_LAYOUT_OFFSET));
			offset_row ++;
		}
		if (rva_row <= rva->rows && (mono_metadata_decode_row_col (rva, rva_row - 1, MONO_FIELD_RVA_FIELD) == i)) {
			fprintf (output, "\trva: %d\n", mono_metadata_decode_row_col (rva, rva_row - 1, MONO_FIELD_RVA_RVA));
			rva_row ++;
		}
	}
	fprintf (output, "\n");
}

void
dump_table_memberref (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_MEMBERREF];
	int i, kind, idx;
	char *x, *xx;
	char *sig;
	const char *blob, *ks;

	fprintf (output, "MemberRef Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_MEMBERREF_SIZE];

		mono_metadata_decode_row (t, i, cols, MONO_MEMBERREF_SIZE);
		
		kind = cols [MONO_MEMBERREF_CLASS] & 7;
		idx = cols [MONO_MEMBERREF_CLASS] >> 3;

		x = g_strdup ("UNHANDLED CASE");
		
		switch (kind){
		case 0:
			ks = "TypeDef"; break;
		case 1:
			ks = "TypeRef";
			xx = get_typeref (m, idx);
			x = g_strconcat (xx, ".", mono_metadata_string_heap (m, cols [MONO_MEMBERREF_NAME]), NULL);
			g_free (xx);
			break;
		case 2:
			ks = "ModuleRef"; break;
		case 3:
			ks = "MethodDef"; break;
		case 4:
			ks = "TypeSpec";
			xx = get_typespec (m, idx);
			x = g_strconcat (xx, ".", mono_metadata_string_heap (m, cols [MONO_MEMBERREF_NAME]), NULL);
			g_free (xx);
			break;
		default:
			g_error ("Unknown tag: %d\n", kind);
		}
		blob = mono_metadata_blob_heap (m, cols [MONO_MEMBERREF_SIGNATURE]);
		mono_metadata_decode_blob_size (blob, &blob);
		if (*blob == 0x6) { /* it's a field */
			sig = get_field_signature (m, cols [MONO_MEMBERREF_SIGNATURE]);
		} else {
			sig = get_methodref_signature (m, cols [MONO_MEMBERREF_SIGNATURE], NULL);
		}
		fprintf (output, "%d: %s[%d] %s\n\tResolved: %s\n\tSignature: %s\n\t\n",
			 i + 1,
			 ks, idx,
			 mono_metadata_string_heap (m, cols [MONO_MEMBERREF_NAME]),
			 x ? x : "",
			 sig);

		if (x)
			g_free (x);
		g_free (sig);
	}
}

void
dump_table_class_layout (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_CLASSLAYOUT];
	int i;
	fprintf (output, "ClassLayout Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_CLASS_LAYOUT_SIZE];
		
		mono_metadata_decode_row (t, i, cols, CSIZE (cols));

		fprintf (output, "%d: PackingSize=%d  ClassSize=%d  Parent=%s\n",
			 i + 1, cols [MONO_CLASS_LAYOUT_PACKING_SIZE], 
			 cols [MONO_CLASS_LAYOUT_CLASS_SIZE], 
			 get_typedef (m, cols [MONO_CLASS_LAYOUT_PARENT]));
	}
}

void
dump_table_constant (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_CONSTANT];
	int i;
	const char *desc [] = {
		"Field",
		"Param",
		"Property",
		""
	};
	fprintf (output, "Constant Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_CONSTANT_SIZE];
		const char *parent = desc [cols [MONO_CONSTANT_PARENT] & HASCONSTANT_MASK];
		
		mono_metadata_decode_row (t, i, cols, MONO_CONSTANT_SIZE);

		fprintf (output, "%d: Parent= %s: %d %s\n",
			 i + 1, parent, cols [MONO_CONSTANT_PARENT] >> HASCONSTANT_BITS, 
			 get_constant (m, (MonoTypeEnum) cols [MONO_CONSTANT_TYPE], cols [MONO_CONSTANT_VALUE]));
	}
	
}

void
dump_table_property_map (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_PROPERTYMAP];
	int i;
	char *s;
	
	fprintf (output, "Property Map Table (1..%d)\n", t->rows);
	
	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_PROPERTY_MAP_SIZE];
		
		mono_metadata_decode_row (t, i, cols, MONO_PROPERTY_MAP_SIZE);
		s = get_typedef (m, cols [MONO_PROPERTY_MAP_PARENT]);
		fprintf (output, "%d: %s (%d) %d\n", i + 1, s, cols [MONO_PROPERTY_MAP_PARENT], cols [MONO_PROPERTY_MAP_PROPERTY_LIST]);
		g_free (s);
	}
}

void
dump_table_property (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_PROPERTY];
	int i, j, pcount;
	const char *ptr;
	char flags[128];

	fprintf (output, "Property Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_PROPERTY_SIZE];
		char *type;
		int bsize;
		int prop_flags;
		
		mono_metadata_decode_row (t, i, cols, MONO_PROPERTY_SIZE);
		flags [0] = 0;
		prop_flags = cols [MONO_PROPERTY_FLAGS];
		if (prop_flags & 0x0200)
			strcat (flags, "special ");
		if (prop_flags & 0x0400)
			strcat (flags, "runtime ");
		if (prop_flags & 0x1000)
			strcat (flags, "hasdefault ");

		ptr = mono_metadata_blob_heap (m, cols [MONO_PROPERTY_TYPE]);
		bsize = mono_metadata_decode_blob_size (ptr, &ptr);
		/* ECMA claims 0x08 ... */
		if (*ptr != 0x28 && *ptr != 0x08)
				g_warning("incorrect signature in propert blob: 0x%x", *ptr);
		ptr++;
		pcount = mono_metadata_decode_value (ptr, &ptr);
		ptr = get_type (m, ptr, &type);
		fprintf (output, "%d: %s %s (",
			 i + 1, type, mono_metadata_string_heap (m, cols [MONO_PROPERTY_NAME]));
		g_free (type);

		for (j = 0; j < pcount; j++){
				ptr = get_param (m, ptr, &type);
				fprintf (output, "%s%s", j > 0? ", " : "",type);
				g_free (type);
		}
		fprintf (output, ") %s\n", flags);
	}
}

void
dump_table_event (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_EVENT];
	int i;
	fprintf (output, "Event Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_EVENT_SIZE];
		const char *name;
		char *type;
		
		mono_metadata_decode_row (t, i, cols, MONO_EVENT_SIZE);

		name = mono_metadata_string_heap (m, cols [MONO_EVENT_NAME]);
		type = get_typedef_or_ref (m, cols [MONO_EVENT_TYPE]);
		fprintf (output, "%d: %s %s %s\n", i + 1, type, name,
			 cols [MONO_EVENT_FLAGS] & 0x200 ? "specialname " : "");
		g_free (type);
	}
	
}

void
dump_table_file (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_FILE];
	int i, j, len;
	fprintf (output, "File Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_FILE_SIZE];
		const char *name, *hash;
		
		mono_metadata_decode_row (t, i, cols, MONO_FILE_SIZE);

		name = mono_metadata_string_heap (m, cols [MONO_FILE_NAME]);
		fprintf (output, "%d: %s %s [", i + 1, name, 
				cols [MONO_FILE_FLAGS] & 0x1 ? "nometadata" : "containsmetadata");
		hash = mono_metadata_blob_heap (m, cols [MONO_FILE_HASH_VALUE]);
		len = mono_metadata_decode_blob_size (hash, &hash);
		for (j = 0; j < len; ++j)
			fprintf (output, "%s%02X", j? " ": "", hash [j] & 0xff);
		fprintf (output, "]\n");
	}
	
}

static char*
get_manifest_implementation (MonoImage *m, guint32 idx)
{
	guint32 row;
	const char* table = "";
	if (!idx)
		return g_strdup ("current module");
	row = idx >> IMPLEMENTATION_BITS;
	switch (idx & IMPLEMENTATION_MASK) {
	case IMPLEMENTATION_FILE:
		table = "file";
		break;
	case IMPLEMENTATION_ASSEMBLYREF:
		table = "assemblyref";
		break;
	case IMPLEMENTATION_EXP_TYPE:
		table = "exportedtype";
		break;
	default:
		g_assert_not_reached ();
	}
	return g_strdup_printf ("%s %d", table, row);
}

static const char*
get_manifest_flags (guint32 mf)
{
	mf &= 3;
	switch (mf) {
	case 1: return "public";
	case 2: return "private";
	default:
		return "";
	}
}

void
dump_table_manifest (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_MANIFESTRESOURCE];
	int i;
	fprintf (output, "Manifestresource Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_MANIFEST_SIZE];
		const char *name, *mf;
		char *impl;
		
		mono_metadata_decode_row (t, i, cols, MONO_MANIFEST_SIZE);

		name = mono_metadata_string_heap (m, cols [MONO_MANIFEST_NAME]);
		mf = get_manifest_flags (cols [MONO_MANIFEST_FLAGS]);
		impl = get_manifest_implementation (m, cols [MONO_MANIFEST_IMPLEMENTATION]);
		fprintf (output, "%d: %s '%s' at offset %u in %s\n", i + 1, mf, name, cols [MONO_MANIFEST_OFFSET], impl);
		g_free (impl);
	}
	
}

void
dump_table_moduleref (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_MODULEREF];
	int i;
	fprintf (output, "ModuleRef Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_MODULEREF_SIZE];
		const char *name;
		
		mono_metadata_decode_row (t, i, cols, MONO_MODULEREF_SIZE);

		name = mono_metadata_string_heap (m, cols [MONO_MODULEREF_NAME]);
		fprintf (output, "%d: %s\n", i + 1, name);
	}
	
}

void
dump_table_module (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_MODULE];
	int i;
	fprintf (output, "ModuleRef Table (1..%d)\n", t->rows);

	for (i = 0; i < t->rows; i++){
		guint32 cols [MONO_MODULE_SIZE];
		const char *name;
		char *guid;
		
		mono_metadata_decode_row (t, i, cols, MONO_MODULE_SIZE);

		name = mono_metadata_string_heap (m, cols [MONO_MODULE_NAME]);
		guid = get_guid (m, cols [MONO_MODULE_MVID]);
		fprintf (output, "%d: %s %d %s\n", i + 1, name, cols [MONO_MODULE_MVID], guid);
	}
	
}

void
dump_table_method (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_METHOD];
	MonoTableInfo *td = &m->tables [MONO_TABLE_TYPEDEF];
	int i, current_type;
	guint32 first_m, last_m;
	fprintf (output, "Method Table (1..%d)\n", t->rows);

	current_type = 1;
	last_m = first_m = 1;
	for (i = 1; i <= t->rows; i++){
		guint32 cols [MONO_METHOD_SIZE];
		char *sig;
		const char *sigblob;
		MonoMethodSignature *method;

		/*
		 * Find the next type.
		 */
		while (current_type <= td->rows && i >= (last_m = mono_metadata_decode_row_col (td, current_type - 1, MONO_TYPEDEF_METHOD_LIST))) {
			current_type++;
		}
		if (i == first_m) {
			fprintf (output, "########## %s.%s\n",
				mono_metadata_string_heap (m, mono_metadata_decode_row_col (td, current_type - 2, MONO_TYPEDEF_NAMESPACE)),
				mono_metadata_string_heap (m, mono_metadata_decode_row_col (td, current_type - 2, MONO_TYPEDEF_NAME)));
			first_m = last_m;
		}
		mono_metadata_decode_row (t, i - 1, cols, MONO_METHOD_SIZE);
		sigblob = mono_metadata_blob_heap (m, cols [MONO_METHOD_SIGNATURE]);
		mono_metadata_decode_blob_size (sigblob, &sigblob);
		method = mono_metadata_parse_method_signature (m, i, sigblob, &sigblob);
		sig = dis_stringify_method_signature (m, method, i, FALSE);
		fprintf (output, "%d: %s (param: %d)\n", i, sig, cols [MONO_METHOD_PARAMLIST]);
		g_free (sig);
		mono_metadata_free_method_signature (method);
	}
	
}

static guint32
method_dor_to_token (guint32 idx) {
	switch (idx & METHODDEFORREF_MASK) {
	case METHODDEFORREF_METHODDEF:
		return MONO_TOKEN_METHOD_DEF | (idx >> METHODDEFORREF_BITS);
	case METHODDEFORREF_METHODREF:
		return MONO_TOKEN_MEMBER_REF | (idx >> METHODDEFORREF_BITS);
	}
	return -1;
}

void
dump_table_methodimpl (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_METHODIMPL];
	/*MonoTableInfo *td = &m->tables [MONO_TABLE_TYPEDEF];*/
	int i;

	fprintf (output, "MethodImpl Table (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++){
		guint32 cols [MONO_METHODIMPL_SIZE];
		char *class, *impl, *decl;

		mono_metadata_decode_row (t, i - 1, cols, MONO_METHODIMPL_SIZE);
		class = get_typedef (m, cols [MONO_METHODIMPL_CLASS]);
		impl = get_method (m, method_dor_to_token (cols [MONO_METHODIMPL_BODY]));
		decl = get_method (m, method_dor_to_token (cols [MONO_METHODIMPL_DECLARATION]));
		fprintf (output, "%d: %s\n\tdecl: %s\n\timpl: %s\n", i, class, decl, impl);
		g_free (class);
		g_free (impl);
		g_free (decl);
	}
	
}

static map_t semantics_map [] = {
		{1, "setter"},
		{2, "getter"},
		{4, "other"},
		{8, "add-on"},
		{0x10, "remove-on"},
		{0x20, "fire"},
		{0, NULL},
};

void
dump_table_methodsem (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_METHODSEMANTICS];
	int i, is_property, index;
	const char *semantics;
	
	fprintf (output, "Method Semantics Table (1..%d)\n", t->rows);
	for (i = 1; i <= t->rows; i++){
		guint32 cols [MONO_METHOD_SEMA_SIZE];
		
		mono_metadata_decode_row (t, i - 1, cols, MONO_METHOD_SEMA_SIZE);
		semantics = flags (cols [MONO_METHOD_SEMA_SEMANTICS], semantics_map);
		is_property = cols [MONO_METHOD_SEMA_ASSOCIATION] & HAS_SEMANTICS_MASK;
		index = cols [MONO_METHOD_SEMA_ASSOCIATION] >> HAS_SEMANTICS_BITS;
		fprintf (output, "%d: [%d] %s method: %d %s %d\n", i, cols [MONO_METHOD_SEMA_ASSOCIATION], semantics,
						cols [MONO_METHOD_SEMA_METHOD] - 1, 
						is_property? "property" : "event",
						index);
	}
}

void 
dump_table_interfaceimpl (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_INTERFACEIMPL];
	int i;

	fprintf (output, "Interface Implementation Table (1..%d)\n", t->rows);
	for (i = 1; i <= t->rows; i++) {
		guint32 cols [MONO_INTERFACEIMPL_SIZE];
		
		mono_metadata_decode_row (t, i - 1, cols, MONO_INTERFACEIMPL_SIZE);
		fprintf (output, "%d: %s implements %s\n", i,
			get_typedef (m, cols [MONO_INTERFACEIMPL_CLASS]),
			get_typedef_or_ref (m, cols [MONO_INTERFACEIMPL_INTERFACE]));
	}
}

static char*
has_cattr_get_table (MonoImage *m, guint32 val)
{
	guint32 t = val & CUSTOM_ATTR_MASK;
	guint32 index = val >> CUSTOM_ATTR_BITS;
	const char *table;

	switch (t) {
	case CUSTOM_ATTR_METHODDEF:
		table = "MethodDef";
		break;
	case CUSTOM_ATTR_FIELDDEF:
		table = "FieldDef";
		break;
	case CUSTOM_ATTR_TYPEREF:
		table = "TypeRef";
		break;
	case CUSTOM_ATTR_TYPEDEF:
		table = "TypeDef";
		break;
	case CUSTOM_ATTR_PARAMDEF:
		table = "Param";
		break;
	case CUSTOM_ATTR_INTERFACE:
		table = "InterfaceImpl";
		break;
	case CUSTOM_ATTR_MEMBERREF:
		table = "MemberRef";
		break;
	case CUSTOM_ATTR_MODULE:
		table = "Module";
		break;
	case CUSTOM_ATTR_PERMISSION:
		table = "DeclSecurity?";
		break;
	case CUSTOM_ATTR_PROPERTY:
		table = "Property";
		break;
	case CUSTOM_ATTR_EVENT:
		table = "Event";
		break;
	case CUSTOM_ATTR_SIGNATURE:
		table = "StandAloneSignature";
		break;
	case CUSTOM_ATTR_MODULEREF:
		table = "ModuleRef";
		break;
	case CUSTOM_ATTR_TYPESPEC:
		table = "TypeSpec";
		break;
	case CUSTOM_ATTR_ASSEMBLY:
		table = "Assembly";
		break;
	case CUSTOM_ATTR_ASSEMBLYREF:
		table = "AssemblyRef";
		break;
	case CUSTOM_ATTR_FILE:
		table = "File";
		break;
	case CUSTOM_ATTR_EXP_TYPE:
		table = "ExportedType";
		break;
	case CUSTOM_ATTR_MANIFEST:
		table = "Manifest";
		break;
	default:
		table = "Unknown";
		break;
	}
	/*
	 * FIXME: we should decode the index into something more uman-friendly.
	 */
	return g_strdup_printf ("%s: %d", table, index);
}

static char*
custom_attr_params (MonoImage *m, MonoMethodSignature* sig, const char* value)
{
	int len, i, slen, type;
	GString *res;
	char *s;
	const char *p = value;

	len = mono_metadata_decode_value (p, &p);
	if (len < 2 || read16 (p) != 0x0001) /* Prolog */
		return g_strdup ("");

	/* skip prolog */
	p += 2;
	res = g_string_new ("");
	for (i = 0; i < sig->param_count; ++i) {
		if (i != 0)
			g_string_append (res, ", ");
		type = sig->params [i]->type;
handle_enum:
		switch (type) {
		case MONO_TYPE_U1:
			g_string_sprintfa (res, "%d", (unsigned int)*p);
			++p;
			break;
		case MONO_TYPE_I1:
			g_string_sprintfa (res, "%d", *p);
			++p;
			break;
		case MONO_TYPE_BOOLEAN:
			g_string_sprintfa (res, "%s", *p?"true":"false");
			++p;
			break;
		case MONO_TYPE_CHAR:
			g_string_sprintfa (res, "'%c'", read16 (p));
			p += 2;
			break;
		case MONO_TYPE_U2:
			g_string_sprintfa (res, "%d", read16 (p));
			p += 2;
			break;
		case MONO_TYPE_I2:
			g_string_sprintfa (res, "%d", (gint16)read16 (p));
			p += 2;
			break;
		case MONO_TYPE_U4:
			g_string_sprintfa (res, "%d", read32 (p));
			p += 4;
			break;
		case MONO_TYPE_I4:
			g_string_sprintfa (res, "%d", (gint32)read32 (p));
			p += 4;
			break;
		case MONO_TYPE_U8:
			g_string_sprintfa (res, "%lld", read64 (p));
			p += 8;
			break;
		case MONO_TYPE_I8:
			g_string_sprintfa (res, "%lld", (gint64)read64 (p));
			p += 8;
			break;
		case MONO_TYPE_R4: {
			float val;
			readr4 (p, &val);
			g_string_sprintfa (res, "%g", val);
			p += 4;
			break;
		}
		case MONO_TYPE_R8: {
			double val;
			readr8 (p, &val);
			g_string_sprintfa (res, "%g", val);
			p += 8;
			break;
		}
		case MONO_TYPE_VALUETYPE:
			if (sig->params [i]->data.klass->enumtype) {
				type = sig->params [i]->data.klass->enum_basetype->type;
				goto handle_enum;
			} else {
				g_warning ("generic valutype not handled in custom attr value decoding");
			}
			break;
		case MONO_TYPE_CLASS: /* It must be a Type: check? */
		case MONO_TYPE_STRING:
			if (*p == (char)0xff) {
				g_string_append (res, "null");
				p++;
				break;
			}
			slen = mono_metadata_decode_value (p, &p);
			g_string_append_c (res, '"');
			g_string_append (res, p);
			g_string_append_c (res, '"');
			p += slen;
			break;
		default:
			g_warning ("Type %02x not handled in custom attr value decoding", sig->params [i]->type);
			break;
		}
	}
	slen = read16 (p);
	if (slen) {
		g_string_sprintfa (res, " %d named args: (", slen);
		slen = len - (p - value) + 1;
		for (i = 0; i < slen; ++i) {
			g_string_sprintfa (res, " %02X", (p [i] & 0xff));
		}
		g_string_append_c (res, ')');
	}
	s = res->str;
	g_string_free (res, FALSE);
	return s;
}

void
dump_table_customattr (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_CUSTOMATTRIBUTE];
	int i;

	fprintf (output, "Custom Attributes Table (1..%d)\n", t->rows);
	for (i = 1; i <= t->rows; i++) {
		guint32 cols [MONO_CUSTOM_ATTR_SIZE];
		guint32 mtoken;
		char * desc;
		char *method;
		char *params;
		MonoMethod *meth;
		
		mono_metadata_decode_row (t, i - 1, cols, MONO_CUSTOM_ATTR_SIZE);
		desc = has_cattr_get_table (m, cols [MONO_CUSTOM_ATTR_PARENT]);
		mtoken = cols [MONO_CUSTOM_ATTR_TYPE] >> CUSTOM_ATTR_TYPE_BITS;
		switch (cols [MONO_CUSTOM_ATTR_TYPE] & CUSTOM_ATTR_TYPE_MASK) {
		case CUSTOM_ATTR_TYPE_METHODDEF:
			mtoken |= MONO_TOKEN_METHOD_DEF;
			break;
		case CUSTOM_ATTR_TYPE_MEMBERREF:
			mtoken |= MONO_TOKEN_MEMBER_REF;
			break;
		default:
			g_warning ("Unknown table for custom attr type %08x", cols [MONO_CUSTOM_ATTR_TYPE]);
			break;
		}
		method = get_method (m, mtoken);
		meth = mono_get_method (m, mtoken, NULL);
		params = custom_attr_params (m, meth->signature, mono_metadata_blob_heap (m, cols [MONO_CUSTOM_ATTR_VALUE]));
		fprintf (output, "%d: %s: %s [%s]\n", i, desc, method, params);
		g_free (desc);
		g_free (method);
		g_free (params);
	}
}

void
dump_table_nestedclass (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_NESTEDCLASS];
	guint32 cols [MONO_NESTED_CLASS_SIZE];
	int i;
	char *nested, *nesting;
	fprintf (output, "NestedClass Table (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++){
		mono_metadata_decode_row (t, i - 1, cols, MONO_NESTED_CLASS_SIZE);
		nested = get_typedef (m, cols [MONO_NESTED_CLASS_NESTED]);
		nesting = get_typedef (m, cols [MONO_NESTED_CLASS_ENCLOSING]);
		fprintf (output, "%d: %d %d: %s in %s\n", i,
				cols [MONO_NESTED_CLASS_NESTED], 
				cols [MONO_NESTED_CLASS_ENCLOSING], nested, nesting);
		g_free (nested);
		g_free (nesting);
	}
	
}

void
dump_table_exported (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_EXPORTEDTYPE];
	guint32 cols [MONO_EXP_TYPE_SIZE];
	int i;
	const char *name, *nspace;
	char *impl;
	fprintf (output, "ExportedType Table (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++) {
		mono_metadata_decode_row (t, i - 1, cols, MONO_EXP_TYPE_SIZE);
		name = mono_metadata_string_heap (m, cols [MONO_EXP_TYPE_NAME]);
		nspace = mono_metadata_string_heap (m, cols [MONO_EXP_TYPE_NAMESPACE]);
		impl = get_manifest_implementation (m, cols [MONO_EXP_TYPE_IMPLEMENTATION]);
		fprintf (output, "%d: %s.%s is in %s\n", i, nspace, name, impl);
		g_free (impl);
	}
	
}

void
dump_table_field_marshal (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_FIELDMARSHAL];
	guint32 cols [MONO_FIELD_MARSHAL_SIZE];
	int i, is_field, idx;
	const char *blob;
	char *native;
	
	fprintf (output, "FieldMarshal Table (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++) {
		mono_metadata_decode_row (t, i - 1, cols, MONO_FIELD_MARSHAL_SIZE);
		blob = mono_metadata_blob_heap (m, cols [MONO_FIELD_MARSHAL_NATIVE_TYPE]);
		native = get_marshal_info (m, blob);
		is_field = (cols [MONO_FIELD_MARSHAL_PARENT] & HAS_FIELD_MARSHAL_MASK) == HAS_FIELD_MARSHAL_FIELDSREF;
		idx = cols [MONO_FIELD_MARSHAL_PARENT] >> HAS_FIELD_MARSHAL_BITS;
		fprintf (output, "%d: (0x%04x) %s %d: %s\n", i, cols [MONO_FIELD_MARSHAL_PARENT], is_field? "Field" : "Param", idx, native);
		g_free (native);
	}
	
}

static const char*
get_security_action (int val) {
	static char buf [32];

	switch (val) {
	case SECURITY_ACTION_DEMAND:
		return "Demand";
	case SECURITY_ACTION_ASSERT:
		return "Assert";
	case SECURITY_ACTION_DENY:
		return "Deny";
	case SECURITY_ACTION_PERMITONLY:
		return "PermitOnly";
	case SECURITY_ACTION_LINKDEMAND:
		return "LinkDemand";
	case SECURITY_ACTION_INHERITDEMAND:
		return "InheritanceDemand";
	case SECURITY_ACTION_REQMIN:
		return "RequestMinimum";
	case SECURITY_ACTION_REQOPT:
		return "RequestOptional";
	case SECURITY_ACTION_REQREFUSE:
		return "RequestRefuse";
	default:
		g_snprintf (buf, sizeof (buf), "0x%04X", val);
		return buf;
	}
}

void 
dump_table_declsec (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_DECLSECURITY];
	guint32 cols [MONO_DECL_SECURITY_SIZE];
	int i, len;
	guint32 idx;
	const char *blob, *action;
	const char* parent[] = {
		"TypeDef", "MethodDef", "Assembly", ""
	};
	
	fprintf (output, "DeclSecurity Table (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++) {
		mono_metadata_decode_row (t, i - 1, cols, MONO_DECL_SECURITY_SIZE);
		blob = mono_metadata_blob_heap (m, cols [MONO_DECL_SECURITY_PERMISSIONSET]);
		len = mono_metadata_decode_blob_size (blob, &blob);
		action = get_security_action (cols [MONO_DECL_SECURITY_ACTION]);
		idx = cols [MONO_DECL_SECURITY_PARENT];
		fprintf (output, "%d: %s on %s %d%s", i, action, parent [idx & HAS_DECL_SECURITY_MASK], idx >> HAS_DECL_SECURITY_BITS, len? ":\n\t":"\n");
		if (!len)
			continue;
		for (idx = 0; idx < len; ++idx)
			fprintf (output, "%c", blob [idx]);
		fprintf (output, "\n");
	}
}

void 
dump_table_genericpar (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_GENERICPARAM];
	guint32 cols [MONO_GENERICPARAM_SIZE];
	int i;

	fprintf (output, "GenericParameters (1..%d)\n", t->rows);
        
	for (i = 1; i <= t->rows; i++) {
                char *sig;
		mono_metadata_decode_row (t, i - 1, cols, MONO_GENERICPARAM_SIZE);

                sig = get_type_or_methdef (m, cols [MONO_GENERICPARAM_OWNER]);
		fprintf (output, "%d: %d, flags=%d, owner=%s %s\n", i,
			 cols [MONO_GENERICPARAM_NUMBER],
			 cols [MONO_GENERICPARAM_FLAGS], sig,
			 mono_metadata_string_heap (m, cols [MONO_GENERICPARAM_NAME]));
                g_free (sig);
	}
}

void
dump_table_methodspec (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_METHODSPEC];
	guint32 cols [MONO_METHODSPEC_SIZE];
	int i;
	
	fprintf (output, "MethodSpec (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++) {
		char *sig;
		char *method;
                guint32 token;
                
		mono_metadata_decode_row (t, i - 1, cols, MONO_METHODSPEC_SIZE);

                // build a methodspec token to get the method
                token = MONO_TOKEN_METHOD_SPEC | i;
                method = get_method (m, token);
                
                sig = get_method_type_param (m, cols [MONO_METHODSPEC_SIGNATURE]);
		fprintf (output, "%d: %s, %s\n", i, method, sig);
		g_free (sig);
		g_free (method);
	}
}

void
dump_table_parconstraint (MonoImage *m)
{
	MonoTableInfo *t = &m->tables [MONO_TABLE_GENERICPARAMCONSTRAINT];
	guint32 cols [MONO_GENPARCONSTRAINT_SIZE];
	int i;
	
	fprintf (output, "Generic Param Constraint (1..%d)\n", t->rows);

	for (i = 1; i <= t->rows; i++) {
                char *sig;
		mono_metadata_decode_row (t, i - 1, cols, MONO_GENPARCONSTRAINT_SIZE);

                sig = get_typedef_or_ref (m, cols [MONO_GENPARCONSTRAINT_CONSTRAINT]);
		fprintf (output, "%d: gen-par=%d, Constraint=%s\n", i,
			 cols [MONO_GENPARCONSTRAINT_GENERICPAR], sig);
                g_free (sig);
	}
}

