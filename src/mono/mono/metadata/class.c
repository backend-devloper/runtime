/*
 * class.c: Class management for the Mono runtime
 *
 * Author:
 *   Miguel de Icaza (miguel@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 *
 * Possible Optimizations:
 *     in mono_class_create, do not allocate the class right away,
 *     but wait until you know the size of the FieldMap, so that
 *     the class embeds directly the FieldMap after the vtable.
 *
 * 
 */
#include <config.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <mono/metadata/image.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/cil-coff.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/reflection.h>
#include <mono/os/gc_wrapper.h>

MonoStats mono_stats;

gboolean mono_print_vtable = FALSE;

static MonoClass * mono_class_create_from_typedef (MonoImage *image, guint32 type_token);

void (*mono_debugger_class_init_func) (MonoClass *klass) = NULL;

MonoClass *
mono_class_from_typeref (MonoImage *image, guint32 type_token)
{
	guint32 cols [MONO_TYPEREF_SIZE];
	MonoTableInfo  *t = &image->tables [MONO_TABLE_TYPEREF];
	guint32 idx;
	const char *name, *nspace;
	MonoClass *res;
	MonoAssembly **references;

	mono_metadata_decode_row (t, (type_token&0xffffff)-1, cols, MONO_TYPEREF_SIZE);

	name = mono_metadata_string_heap (image, cols [MONO_TYPEREF_NAME]);
	nspace = mono_metadata_string_heap (image, cols [MONO_TYPEREF_NAMESPACE]);
	
	idx = cols [MONO_TYPEREF_SCOPE] >> RESOLTION_SCOPE_BITS;
	switch (cols [MONO_TYPEREF_SCOPE] & RESOLTION_SCOPE_MASK) {
	case RESOLTION_SCOPE_MODULE:
		if (!idx)
			g_error ("null ResolutionScope not yet handled");
		/* a typedef in disguise */
		return mono_class_from_name (image, nspace, name);
	case RESOLTION_SCOPE_MODULEREF:
		return mono_class_from_name (image->modules [idx - 1], nspace, name);
	case RESOLTION_SCOPE_TYPEREF: {
		MonoClass *enclosing = mono_class_from_typeref (image, MONO_TOKEN_TYPE_REF | idx);
		GList *tmp;
		mono_class_init (enclosing);
		for (tmp = enclosing->nested_classes; tmp; tmp = tmp->next) {
			res = tmp->data;
			if (strcmp (res->name, name) == 0)
				return res;
		}
		g_warning ("TypeRef ResolutionScope not yet handled (%d)", idx);
		return NULL;
	}
	case RESOLTION_SCOPE_ASSEMBLYREF:
		break;
	}

	references = image->references;
	if (!references ||  !references [idx-1]) {
		/* 
		 * detected a reference to mscorlib, we simply return a reference to a dummy 
		 * until we have a better solution.
		 * 
		 * once a better solution is in place, the System.MonoDummy
		 * class should be removed from CVS.
		 */
		fprintf(stderr, "Sending dummy where %s.%s expected\n", mono_metadata_string_heap (image, cols [MONO_TYPEREF_NAMESPACE]), mono_metadata_string_heap (image, cols [MONO_TYPEREF_NAME])); 
		
		res = mono_class_from_name (image, "System", "MonoDummy");
		/* prevent method loading */
		res->dummy = 1;
		/* some storage if the type is used  - very ugly hack */
		res->instance_size = 2*sizeof (gpointer);
		return res;
	}	

	/* load referenced assembly */
	image = references [idx-1]->image;

	return mono_class_from_name (image, nspace, name);
}

static MonoType*
dup_type (MonoType* t, const MonoType *original)
{
	MonoType *r = g_new0 (MonoType, 1);
	*r = *t;
	r->attrs = original->attrs;
	mono_stats.generics_metadata_size += sizeof (MonoType);
	return r;
}

static void
mono_type_get_name_recurse (MonoType *type, GString *str, gboolean is_recursed)
{
	MonoClass *klass;
	
	switch (type->type) {
	case MONO_TYPE_ARRAY: {
		int i, rank = type->data.array->rank;

		mono_type_get_name_recurse (&type->data.array->eklass->byval_arg, str, FALSE);
		g_string_append_c (str, '[');
		for (i = 1; i < rank; i++)
			g_string_append_c (str, ',');
		g_string_append_c (str, ']');
		break;
	}
	case MONO_TYPE_SZARRAY:
		mono_type_get_name_recurse (&type->data.klass->byval_arg, str, FALSE);
		g_string_append (str, "[]");
		break;
	case MONO_TYPE_PTR:
		mono_type_get_name_recurse (type->data.type, str, FALSE);
		g_string_append_c (str, '*');
		break;
	default:
		klass = mono_class_from_mono_type (type);
		if (klass->nested_in) {
			mono_type_get_name_recurse (&klass->nested_in->byval_arg, str, TRUE);
			g_string_append_c (str, '+');
		}
		if (*klass->name_space) {
			g_string_append (str, klass->name_space);
			g_string_append_c (str, '.');
		}
		g_string_append (str, klass->name);
		if (is_recursed)
			break;
		if (klass->generic_inst) {
			MonoGenericInst *ginst = klass->generic_inst;
			int i;

			g_string_append_c (str, '[');
			for (i = 0; i < ginst->type_argc; i++) {
				if (i)
					g_string_append_c (str, ',');
				mono_type_get_name_recurse (ginst->type_argv [i], str, FALSE);
			}
			g_string_append_c (str, ']');
		} else if (klass->gen_params) {
			int i;

			g_string_append_c (str, '[');
			for (i = 0; i < klass->num_gen_params; i++) {
				if (i)
					g_string_append_c (str, ',');
				g_string_append (str, klass->gen_params [i].name);
			}
			g_string_append_c (str, ']');
		}
		break;
	}
}

/*
 * mono_type_get_name:
 * @type: a type
 *
 * Returns the string representation for type as required by System.Reflection.
 * The inverse of mono_reflection_parse_type ().
 */
char*
mono_type_get_name (MonoType *type)
{
	GString* result = g_string_new ("");
	mono_type_get_name_recurse (type, result, FALSE);

	if (type->byref)
		g_string_append_c (result, '&');

	return g_string_free (result, FALSE);
}

gboolean
mono_class_is_open_constructed_type (MonoType *t)
{
	switch (t->type) {
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return TRUE;
	case MONO_TYPE_SZARRAY:
		return mono_class_is_open_constructed_type (&t->data.klass->byval_arg);
	case MONO_TYPE_ARRAY:
		return mono_class_is_open_constructed_type (&t->data.array->eklass->byval_arg);
	case MONO_TYPE_PTR:
		return mono_class_is_open_constructed_type (t->data.type);
	case MONO_TYPE_GENERICINST: {
		MonoGenericInst *ginst = t->data.generic_inst;
		int i;

		if (mono_class_is_open_constructed_type (ginst->generic_type))
			return TRUE;
		for (i = 0; i < ginst->type_argc; i++)
			if (mono_class_is_open_constructed_type (ginst->type_argv [i]))
				return TRUE;
		return FALSE;
	}
	default:
		return FALSE;
	}
}

static MonoType*
inflate_generic_type (MonoType *type, MonoGenericContext *context)
{
	switch (type->type) {
	case MONO_TYPE_MVAR:
		if (context->gmethod && context->gmethod->mtype_argv)
			return dup_type (
				context->gmethod->mtype_argv [type->data.generic_param->num],
				type);
		else
			return NULL;
	case MONO_TYPE_VAR:
		if (context->ginst)
			return dup_type (
				context->ginst->type_argv [type->data.generic_param->num],
				type);
		else
			return NULL;
	case MONO_TYPE_SZARRAY: {
		MonoClass *eclass = type->data.klass;
		MonoType *nt, *inflated = inflate_generic_type (&eclass->byval_arg, context);
		if (!inflated)
			return NULL;
		nt = dup_type (type, type);
		nt->data.klass = mono_class_from_mono_type (inflated);
		return nt;
	}
	case MONO_TYPE_GENERICINST: {
		MonoGenericInst *oginst = type->data.generic_inst;
		MonoGenericInst *nginst;
		MonoType *nt;
		int i;

		nginst = g_new0 (MonoGenericInst, 1);
		*nginst = *oginst;

		nginst->is_open = FALSE;

		nginst->type_argv = g_new0 (MonoType *, oginst->type_argc);

		for (i = 0; i < oginst->type_argc; i++) {
			MonoType *t = oginst->type_argv [i];
			nginst->type_argv [i] = mono_class_inflate_generic_type (t, context);

			if (!nginst->is_open)
				nginst->is_open = mono_class_is_open_constructed_type (nginst->type_argv [i]);
		};

		nginst->klass = NULL;

		nginst->context = g_new0 (MonoGenericContext, 1);
		nginst->context->ginst = nginst;

		mono_loader_lock ();
		nt = g_hash_table_lookup (oginst->klass->image->generic_inst_cache, nginst);

		if (nt) {
			g_free (nginst->type_argv);
			g_free (nginst);
			mono_loader_unlock ();
			return nt;
		}

		nginst->dynamic_info = NULL;
		nginst->initialized = FALSE;

		mono_class_create_generic (nginst);

		mono_stats.generic_instance_count++;
		mono_stats.generics_metadata_size += sizeof (MonoGenericInst) +
			sizeof (MonoGenericContext) +
			nginst->type_argc * sizeof (MonoType);

		nt = dup_type (type, type);
		nt->data.generic_inst = nginst;
		g_hash_table_insert (oginst->klass->image->generic_inst_cache, nginst, nt);
		mono_loader_unlock ();
		return nt;
	}
	default:
		return NULL;
	}
	return NULL;
}

MonoType*
mono_class_inflate_generic_type (MonoType *type, MonoGenericContext *context)
{
	MonoType *inflated = inflate_generic_type (type, context);

	if (!inflated)
		return type;

	mono_stats.inflated_type_count++;
	return inflated;
}

static MonoMethodSignature*
inflate_generic_signature (MonoImage *image, MonoMethodSignature *sig,
			   MonoGenericContext *context)
{
	MonoMethodSignature *res;
	int i;
	res = mono_metadata_signature_alloc (image, sig->param_count);
	res->ret = mono_class_inflate_generic_type (sig->ret, context);
	for (i = 0; i < sig->param_count; ++i)
		res->params [i] = mono_class_inflate_generic_type (sig->params [i], context);
	res->hasthis = sig->hasthis;
	res->explicit_this = sig->explicit_this;
	res->call_convention = sig->call_convention;
	res->generic_param_count = sig->generic_param_count;
	res->is_inflated = 1;
	return res;
}

static MonoMethodHeader*
inflate_generic_header (MonoMethodHeader *header, MonoGenericContext *context)
{
	MonoMethodHeader *res;
	int i;
	res = g_malloc0 (sizeof (MonoMethodHeader) + sizeof (gpointer) * header->num_locals);
	res->code = header->code;
	res->code_size = header->code_size;
	res->max_stack = header->max_stack;
	res->num_clauses = header->num_clauses;
	res->init_locals = header->init_locals;
	res->num_locals = header->num_locals;
	res->clauses = header->clauses;
	res->gen_params = header->gen_params;
	for (i = 0; i < header->num_locals; ++i)
		res->locals [i] = mono_class_inflate_generic_type (header->locals [i], context);
	return res;
}

MonoMethod*
mono_class_inflate_generic_method (MonoMethod *method, MonoGenericContext *context,
				   MonoClass *klass)
{
	MonoMethodInflated *result;

	if ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
	    (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		return method;

	mono_stats.inflated_method_count++;
	mono_stats.generics_metadata_size +=
		sizeof (MonoMethodInflated) - sizeof (MonoMethodNormal);

	result = g_new0 (MonoMethodInflated, 1);
	result->nmethod = *(MonoMethodNormal*)method;

	if (result->nmethod.header)
		result->nmethod.header = inflate_generic_header (
			result->nmethod.header, context);

	if (klass)
		result->nmethod.method.klass = klass;
	else {
		MonoType *declaring = mono_class_inflate_generic_type (
			&method->klass->byval_arg, context);
		result->nmethod.method.klass = mono_class_from_mono_type (declaring);
	}

	result->nmethod.method.signature = inflate_generic_signature (
		method->klass->image, method->signature, context);

	if (context->gmethod) {
		result->context = g_new0 (MonoGenericContext, 1);
		result->context->gmethod = context->gmethod;
		result->context->ginst = result->nmethod.method.klass->generic_inst;

		mono_stats.generics_metadata_size += sizeof (MonoGenericContext);
	} else
		result->context = result->nmethod.method.klass->generic_inst->context;

	if (method->signature->is_inflated)
		result->declaring = ((MonoMethodInflated *) method)->declaring;
	else
		result->declaring = method;

	return (MonoMethod *) result;
}

/** 
 * class_compute_field_layout:
 * @m: pointer to the metadata.
 * @class: The class to initialize
 *
 * Initializes the class->fields.
 *
 * Currently we only support AUTO_LAYOUT, and do not even try to do
 * a good job at it.  This is temporary to get the code for Paolo.
 */
static void
class_compute_field_layout (MonoClass *class)
{
	MonoImage *m = class->image; 
	const int top = class->field.count;
	guint32 layout = class->flags & TYPE_ATTRIBUTE_LAYOUT_MASK;
	MonoTableInfo *t = &m->tables [MONO_TABLE_FIELD];
	int i, blittable = TRUE, real_size = 0;
	guint32 rva;
	guint32 packing_size = 0;
	gboolean explicit_size;
	MonoClassField *field;

	if (class->size_inited)
		return;

	if (class->parent) {
		if (!class->parent->size_inited)
			class_compute_field_layout (class->parent);
		class->instance_size += class->parent->instance_size;
		class->min_align = class->parent->min_align;
		blittable = class->blittable;
	} else {
		class->instance_size = sizeof (MonoObject);
		class->min_align = 1;
	}

	/* Get the real size */
	explicit_size = mono_metadata_packing_from_typedef (class->image, class->type_token, &packing_size, &real_size);

	if (explicit_size) {
		g_assert ((packing_size & 0xfffffff0) == 0);
		class->packing_size = packing_size;
		real_size += class->instance_size;
	}

	if (!top) {
		if (explicit_size && real_size) {
			class->instance_size = MAX (real_size, class->instance_size);
		}
		class->size_inited = 1;
		return;
	}

	class->fields = g_new0 (MonoClassField, top);

	/*
	 * Fetch all the field information.
	 */
	for (i = 0; i < top; i++){
		const char *sig;
		guint32 cols [MONO_FIELD_SIZE];
		int idx = class->field.first + i;

		field = &class->fields [i];
		mono_metadata_decode_row (t, idx, cols, MONO_FIELD_SIZE);
		/* The name is needed for fieldrefs */
		field->name = mono_metadata_string_heap (m, cols [MONO_FIELD_NAME]);
		sig = mono_metadata_blob_heap (m, cols [MONO_FIELD_SIGNATURE]);
		mono_metadata_decode_value (sig, &sig);
		/* FIELD signature == 0x06 */
		g_assert (*sig == 0x06);
		field->type = mono_metadata_parse_field_type (
			m, cols [MONO_FIELD_FLAGS], sig + 1, &sig);
		if (mono_field_is_deleted (field))
			continue;
		if (class->generic_inst) {
			field->type = mono_class_inflate_generic_type (
				field->type, class->generic_inst->context);
			field->type->attrs = cols [MONO_FIELD_FLAGS];
		}

		field->parent = class;

		if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC)) {
			if (field->type->byref) {
				blittable = FALSE;
			} else {
				MonoClass *field_class = mono_class_from_mono_type (field->type);
				if (!field_class || !field_class->blittable)
					blittable = FALSE;
			}
		}
		if (layout == TYPE_ATTRIBUTE_EXPLICIT_LAYOUT) {
			mono_metadata_field_info (m, idx, &field->offset, NULL, NULL);
			if (field->offset == (guint32)-1)
				g_warning ("%s not initialized correctly (missing field layout info for %s)", class->name, field->name);
		}

		if (cols [MONO_FIELD_FLAGS] & FIELD_ATTRIBUTE_HAS_FIELD_RVA) {
			mono_metadata_field_info (m, idx, NULL, &rva, NULL);
			if (!rva)
				g_warning ("field %s in %s should have RVA data, but hasn't", field->name, class->name);
			field->data = mono_cli_rva_map (class->image->image_info, rva);
		}

		if (class->enumtype && !(cols [MONO_FIELD_FLAGS] & FIELD_ATTRIBUTE_STATIC)) {
			class->enum_basetype = field->type;
			class->cast_class = class->element_class = mono_class_from_mono_type (class->enum_basetype);
			blittable = class->element_class->blittable;
		}

		/* The def_value of fields is compute lazily during vtable creation */
	}

	if (class == mono_defaults.string_class)
		blittable = FALSE;

	class->blittable = blittable;

	if (class->enumtype && !class->enum_basetype) {
		if (!((strcmp (class->name, "Enum") == 0) && (strcmp (class->name_space, "System") == 0)))
			G_BREAKPOINT ();
	}
	if (explicit_size && real_size) {
		class->instance_size = MAX (real_size, class->instance_size);
	}

	if (class->gen_params)
		return;

	mono_class_layout_fields (class);
}

void
mono_class_layout_fields (MonoClass *class)
{
	int i;
	const int top = class->field.count;
	guint32 layout = class->flags & TYPE_ATTRIBUTE_LAYOUT_MASK;
	guint32 pass, passes, real_size;
	gboolean gc_aware_layout = FALSE;
	MonoClassField *field;

	/*
	 * Enable GC aware auto layout: in this mode, reference
	 * fields are grouped together inside objects, increasing collector 
	 * performance.
	 * Requires that all classes whose layout is known to native code be annotated
	 * with [StructLayout (LayoutKind.Sequential)]
	 */
	 /* corlib is missing [StructLayout] directives in many places */
	if (layout == TYPE_ATTRIBUTE_AUTO_LAYOUT) {
		if (class->image != mono_defaults.corlib)
			gc_aware_layout = TRUE;
	}

	/*
	 * Compute field layout and total size (not considering static fields)
	 */

	switch (layout) {
	case TYPE_ATTRIBUTE_AUTO_LAYOUT:
	case TYPE_ATTRIBUTE_SEQUENTIAL_LAYOUT:

		if (gc_aware_layout)
			passes = 2;
		else
			passes = 1;

		if (layout != TYPE_ATTRIBUTE_AUTO_LAYOUT)
			passes = 1;

		if (class->parent)
			real_size = class->parent->instance_size;
		else
			real_size = sizeof (MonoObject);

		for (pass = 0; pass < passes; ++pass) {
			for (i = 0; i < top; i++){
				int size, align;
				field = &class->fields [i];

				if (mono_field_is_deleted (field))
					continue;
				if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
					continue;

				if (gc_aware_layout) {
					/* 
					 * We process fields with reference type in the first pass,
					 * and fields with non-reference type in the second pass.
					 * We use IS_POINTER instead of IS_REFERENCE because in
					 * some internal structures, we store GC_MALLOCed memory
					 * in IntPtr fields...
					 */
					if (MONO_TYPE_IS_POINTER (field->type)) {
						if (pass == 1)
							continue;
					} else {
						if (pass == 0)
							continue;
					}
				}

				if ((top == 1) && (class->instance_size == sizeof (MonoObject)) &&
					(strcmp (field->name, "$PRIVATE$") == 0)) {
					/* This field is a hack inserted by MCS to empty structures */
					continue;
				}

				size = mono_type_size (field->type, &align);
			
				/* FIXME (LAMESPEC): should we also change the min alignment according to pack? */
				align = class->packing_size ? MIN (class->packing_size, align): align;
				class->min_align = MAX (align, class->min_align);
				field->offset = real_size;
				field->offset += align - 1;
				field->offset &= ~(align - 1);
				real_size = field->offset + size;
			}

			class->instance_size = MAX (real_size, class->instance_size);
       
			if (class->instance_size & (class->min_align - 1)) {
				class->instance_size += class->min_align - 1;
				class->instance_size &= ~(class->min_align - 1);
			}
		}
		break;
	case TYPE_ATTRIBUTE_EXPLICIT_LAYOUT:
		real_size = 0;
		for (i = 0; i < top; i++) {
			int size, align;
			field = &class->fields [i];

			/*
			 * There must be info about all the fields in a type if it
			 * uses explicit layout.
			 */

			if (mono_field_is_deleted (field))
				continue;
			if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			size = mono_type_size (field->type, &align);
			
			/*
			 * When we get here, field->offset is already set by the
			 * loader (for either runtime fields or fields loaded from metadata).
			 * The offset is from the start of the object: this works for both
			 * classes and valuetypes.
			 */
			field->offset += sizeof (MonoObject);

			/*
			 * Calc max size.
			 */
			real_size = MAX (real_size, size + field->offset);
		}
		class->instance_size = MAX (real_size, class->instance_size);
		break;
	}

	class->size_inited = 1;

	/*
	 * Compute static field layout and size
	 */
	for (i = 0; i < top; i++){
		int size, align;
		field = &class->fields [i];
			
		if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC))
			continue;
		if (mono_field_is_deleted (field))
			continue;
			
		size = mono_type_size (field->type, &align);
		field->offset = class->class_size;
		field->offset += align - 1;
		field->offset &= ~(align - 1);
		class->class_size = field->offset + size;
	}
}

static void
init_properties (MonoClass *class)
{
	guint startm, endm, i, j;
	guint32 cols [MONO_PROPERTY_SIZE];
	MonoTableInfo *pt = &class->image->tables [MONO_TABLE_PROPERTY];
	MonoTableInfo *msemt = &class->image->tables [MONO_TABLE_METHODSEMANTICS];

	class->property.first = mono_metadata_properties_from_typedef (class->image, mono_metadata_token_index (class->type_token) - 1, &class->property.last);
	class->property.count = class->property.last - class->property.first;

	class->properties = g_new0 (MonoProperty, class->property.count);
	for (i = class->property.first; i < class->property.last; ++i) {
		mono_metadata_decode_row (pt, i, cols, MONO_PROPERTY_SIZE);
		class->properties [i - class->property.first].parent = class;
		class->properties [i - class->property.first].attrs = cols [MONO_PROPERTY_FLAGS];
		class->properties [i - class->property.first].name = mono_metadata_string_heap (class->image, cols [MONO_PROPERTY_NAME]);

		startm = mono_metadata_methods_from_property (class->image, i, &endm);
		for (j = startm; j < endm; ++j) {
			mono_metadata_decode_row (msemt, j, cols, MONO_METHOD_SEMA_SIZE);
			switch (cols [MONO_METHOD_SEMA_SEMANTICS]) {
			case METHOD_SEMANTIC_SETTER:
				class->properties [i - class->property.first].set = class->methods [cols [MONO_METHOD_SEMA_METHOD] - 1 - class->method.first];
				break;
			case METHOD_SEMANTIC_GETTER:
				class->properties [i - class->property.first].get = class->methods [cols [MONO_METHOD_SEMA_METHOD] - 1 - class->method.first];
				break;
			default:
				break;
			}
		}
	}
}

static void
init_events (MonoClass *class)
{
	guint startm, endm, i, j;
	guint32 cols [MONO_EVENT_SIZE];
	MonoTableInfo *pt = &class->image->tables [MONO_TABLE_EVENT];
	MonoTableInfo *msemt = &class->image->tables [MONO_TABLE_METHODSEMANTICS];

	class->event.first = mono_metadata_events_from_typedef (class->image, mono_metadata_token_index (class->type_token) - 1, &class->event.last);
	class->event.count = class->event.last - class->event.first;

	class->events = g_new0 (MonoEvent, class->event.count);
	for (i = class->event.first; i < class->event.last; ++i) {
		mono_metadata_decode_row (pt, i, cols, MONO_EVENT_SIZE);
		class->events [i - class->event.first].parent = class;
		class->events [i - class->event.first].attrs = cols [MONO_EVENT_FLAGS];
		class->events [i - class->event.first].name = mono_metadata_string_heap (class->image, cols [MONO_EVENT_NAME]);

		startm = mono_metadata_methods_from_event (class->image, i, &endm);
		for (j = startm; j < endm; ++j) {
			mono_metadata_decode_row (msemt, j, cols, MONO_METHOD_SEMA_SIZE);
			switch (cols [MONO_METHOD_SEMA_SEMANTICS]) {
			case METHOD_SEMANTIC_ADD_ON:
				class->events [i - class->event.first].add = class->methods [cols [MONO_METHOD_SEMA_METHOD] - 1 - class->method.first];
				break;
			case METHOD_SEMANTIC_REMOVE_ON:
				class->events [i - class->event.first].remove = class->methods [cols [MONO_METHOD_SEMA_METHOD] - 1 - class->method.first];
				break;
			case METHOD_SEMANTIC_FIRE:
				class->events [i - class->event.first].raise = class->methods [cols [MONO_METHOD_SEMA_METHOD] - 1 - class->method.first];
				break;
			case METHOD_SEMANTIC_OTHER: /* don't care for now */
			default:
				break;
			}
		}
	}
}

static guint
mono_get_unique_iid (MonoClass *class)
{
	static GHashTable *iid_hash = NULL;
	static guint iid = 0;

	char *str;
	gpointer value;
	
	g_assert (MONO_CLASS_IS_INTERFACE (class));

	mono_loader_lock ();

	if (!iid_hash)
		iid_hash = g_hash_table_new (g_str_hash, g_str_equal);

	str = g_strdup_printf ("%s|%s.%s\n", class->image->name, class->name_space, class->name);

	if (g_hash_table_lookup_extended (iid_hash, str, NULL, &value)) {
		mono_loader_unlock ();
		g_free (str);
		return (guint)value;
	} else {
		g_hash_table_insert (iid_hash, str, (gpointer)iid);
		++iid;
	}

	mono_loader_unlock ();

	return iid - 1;
}

static void
collect_implemented_interfaces_aux (MonoClass *klass, GPtrArray **res)
{
	int i;
	MonoClass *ic;
	
	for (i = 0; i < klass->interface_count; i++) {
		ic = klass->interfaces [i];

		if (*res == NULL)
			*res = g_ptr_array_new ();
		g_ptr_array_add (*res, ic);

		collect_implemented_interfaces_aux (ic, res);
	}
}

static inline GPtrArray*
collect_implemented_interfaces (MonoClass *klass)
{
	GPtrArray *res = NULL;

	collect_implemented_interfaces_aux (klass, &res);
	return res;
}

static int
setup_interface_offsets (MonoClass *class, int cur_slot)
{
	MonoClass *k, *ic;
	int i, max_iid;
	GPtrArray *ifaces;

	/* compute maximum number of slots and maximum interface id */
	max_iid = 0;
	for (k = class; k ; k = k->parent) {
		for (i = 0; i < k->interface_count; i++) {
			ic = k->interfaces [i];

			if (!ic->inited)
				mono_class_init (ic);

			if (max_iid < ic->interface_id)
				max_iid = ic->interface_id;
		}
	}

	if (MONO_CLASS_IS_INTERFACE (class)) {
		if (max_iid < class->interface_id)
			max_iid = class->interface_id;
	}
	class->max_interface_id = max_iid;
	/* compute vtable offset for interfaces */
	class->interface_offsets = g_malloc (sizeof (gpointer) * (max_iid + 1));

	for (i = 0; i <= max_iid; i++)
		class->interface_offsets [i] = -1;

	ifaces = collect_implemented_interfaces (class);
	if (ifaces) {
		for (i = 0; i < ifaces->len; ++i) {
			ic = g_ptr_array_index (ifaces, i);
			class->interface_offsets [ic->interface_id] = cur_slot;
			cur_slot += ic->method.count;
		}
		g_ptr_array_free (ifaces, TRUE);
	}

	for (k = class->parent; k ; k = k->parent) {
		ifaces = collect_implemented_interfaces (k);
		if (ifaces) {
			for (i = 0; i < ifaces->len; ++i) {
				ic = g_ptr_array_index (ifaces, i);

				if (class->interface_offsets [ic->interface_id] == -1) {
					int io = k->interface_offsets [ic->interface_id];

					g_assert (io >= 0);

					class->interface_offsets [ic->interface_id] = io;
				}
			}
			g_ptr_array_free (ifaces, TRUE);
		}
	}

	if (MONO_CLASS_IS_INTERFACE (class))
		class->interface_offsets [class->interface_id] = cur_slot;

	return cur_slot;
}

void
mono_class_setup_vtable (MonoClass *class, MonoMethod **overrides, int onum)
{
	MonoClass *k, *ic;
	MonoMethod **vtable;
	int i, max_vtsize = 0, max_iid, cur_slot = 0;
	GPtrArray *ifaces;
	MonoGHashTable *override_map = NULL;

	/* setup_vtable() must be called only once on the type */
	if (class->interface_offsets) {
		g_warning ("vtable already computed in %s.%s", class->name_space, class->name);
		return;
	}

	ifaces = collect_implemented_interfaces (class);
	if (ifaces) {
		for (i = 0; i < ifaces->len; i++) {
			MonoClass *ic = g_ptr_array_index (ifaces, i);
			max_vtsize += ic->method.count;
		}
		g_ptr_array_free (ifaces, TRUE);
	}
	
	if (class->parent) {
		max_vtsize += class->parent->vtable_size;
		cur_slot = class->parent->vtable_size;
	}

	max_vtsize += class->method.count;

	vtable = alloca (sizeof (gpointer) * max_vtsize);
	memset (vtable, 0, sizeof (gpointer) * max_vtsize);

	/* printf ("METAINIT %s.%s\n", class->name_space, class->name); */

	cur_slot = setup_interface_offsets (class, cur_slot);
	max_iid = class->max_interface_id;

	if (class->parent && class->parent->vtable_size)
		memcpy (vtable, class->parent->vtable,  sizeof (gpointer) * class->parent->vtable_size);

	/* override interface methods */
	for (i = 0; i < onum; i++) {
		MonoMethod *decl = overrides [i*2];
		if (MONO_CLASS_IS_INTERFACE (decl->klass)) {
			int dslot;
			g_assert (decl->slot != -1);
			dslot = decl->slot + class->interface_offsets [decl->klass->interface_id];
			vtable [dslot] = overrides [i*2 + 1];
			vtable [dslot]->slot = dslot;
			if (!override_map)
				override_map = mono_g_hash_table_new (NULL, NULL);

			mono_g_hash_table_insert (override_map, overrides [i * 2], overrides [i * 2 + 1]);
		}
	}

	for (k = class; k ; k = k->parent) {
		int nifaces = 0;
		ifaces = collect_implemented_interfaces (k);
		if (ifaces)
			nifaces = ifaces->len;
		for (i = 0; i < nifaces; i++) {
			int j, l, io;

			ic = g_ptr_array_index (ifaces, i);
			io = k->interface_offsets [ic->interface_id];

			g_assert (io >= 0);
			g_assert (io <= max_vtsize);

			if (k == class) {
				for (l = 0; l < ic->method.count; l++) {
					MonoMethod *im = ic->methods [l];						

					if (vtable [io + l] && !(vtable [io + l]->flags & METHOD_ATTRIBUTE_ABSTRACT))
						continue;

					for (j = 0; j < class->method.count; ++j) {
						MonoMethod *cm = class->methods [j];
						if (!(cm->flags & METHOD_ATTRIBUTE_VIRTUAL) ||
						    !((cm->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) ||
						    !(cm->flags & METHOD_ATTRIBUTE_NEW_SLOT))
							continue;
						if (!strcmp(cm->name, im->name) && 
						    mono_metadata_signature_equal (cm->signature, im->signature)) {
							g_assert (io + l <= max_vtsize);
							vtable [io + l] = cm;
						}
					}
				}
			} else {
				/* already implemented */
				if (io >= k->vtable_size)
					continue;
			}
				
			for (l = 0; l < ic->method.count; l++) {
				MonoMethod *im = ic->methods [l];						
				MonoClass *k1;

				g_assert (io + l <= max_vtsize);

 				if (vtable [io + l] && !(vtable [io + l]->flags & METHOD_ATTRIBUTE_ABSTRACT))
					continue;
					
				for (k1 = class; k1; k1 = k1->parent) {
					for (j = 0; j < k1->method.count; ++j) {
						MonoMethod *cm = k1->methods [j];

						if (!(cm->flags & METHOD_ATTRIBUTE_VIRTUAL) ||
						    !(cm->flags & METHOD_ATTRIBUTE_PUBLIC))
							continue;
						
						if (!strcmp(cm->name, im->name) && 
						    mono_metadata_signature_equal (cm->signature, im->signature)) {
							g_assert (io + l <= max_vtsize);
							vtable [io + l] = cm;
							break;
						}
						
					}
					g_assert (io + l <= max_vtsize);
					if (vtable [io + l] && !(vtable [io + l]->flags & METHOD_ATTRIBUTE_ABSTRACT))
						break;
				}
			}

			for (l = 0; l < ic->method.count; l++) {
				MonoMethod *im = ic->methods [l];						
				char *qname, *fqname;
				MonoClass *k1;
				
				if (vtable [io + l])
					continue;
					
				qname = g_strconcat (ic->name, ".", im->name, NULL); 
				if (ic->name_space && ic->name_space [0])
					fqname = g_strconcat (ic->name_space, ".", ic->name, ".", im->name, NULL);
				else
					fqname = NULL;

				for (k1 = class; k1; k1 = k1->parent) {
					for (j = 0; j < k1->method.count; ++j) {
						MonoMethod *cm = k1->methods [j];

						if (!(cm->flags & METHOD_ATTRIBUTE_VIRTUAL))
							continue;
					
						if (((fqname && !strcmp (cm->name, fqname)) || !strcmp (cm->name, qname)) &&
						    mono_metadata_signature_equal (cm->signature, im->signature)) {
							g_assert (io + l <= max_vtsize);
							vtable [io + l] = cm;
							break;
						}
					}
				}
				g_free (qname);
				g_free (fqname);
			}

			
			if (!(class->flags & TYPE_ATTRIBUTE_ABSTRACT)) {
				for (l = 0; l < ic->method.count; l++) {
					char *msig;
					MonoMethod *im = ic->methods [l];
					if (im->flags & METHOD_ATTRIBUTE_STATIC)
							continue;
					g_assert (io + l <= max_vtsize);

					/* 
					 * If one of our parents already implements this interface
					 * we can inherit the implementation.
					 */
					if (!(vtable [io + l])) {
						MonoClass *parent = class->parent;

						if ((ic->interface_id <= parent->max_interface_id) && 
							(parent->interface_offsets [ic->interface_id]) &&
							parent->vtable)
							vtable [io + l] = parent->vtable [parent->interface_offsets [ic->interface_id] + l];
					}

					if (!(vtable [io + l])) {
						for (j = 0; j < onum; ++j) {
							g_print (" at slot %d: %s (%d) overrides %s (%d)\n", io+l, overrides [j*2+1]->name, 
								 overrides [j*2+1]->slot, overrides [j*2]->name, overrides [j*2]->slot);
						}
						msig = mono_signature_get_desc (im->signature, FALSE);
						printf ("no implementation for interface method %s.%s::%s(%s) in class %s.%s\n",
							ic->name_space, ic->name, im->name, msig, class->name_space, class->name);
						g_free (msig);
						for (j = 0; j < class->method.count; ++j) {
							MonoMethod *cm = class->methods [j];
							msig = mono_signature_get_desc (cm->signature, FALSE);
							
							printf ("METHOD %s(%s)\n", cm->name, msig);
							g_free (msig);
						}
						g_assert_not_reached ();
					}
				}
			}
		
			for (l = 0; l < ic->method.count; l++) {
				MonoMethod *im = vtable [io + l];

				if (im) {
					g_assert (io + l <= max_vtsize);
					if (im->slot < 0) {
						/* FIXME: why do we need this ? */
						im->slot = io + l;
						/* g_assert_not_reached (); */
					}
				}
			}
		}
		if (ifaces)
			g_ptr_array_free (ifaces, TRUE);
	} 

	for (i = 0; i < class->method.count; ++i) {
		MonoMethod *cm;
	       
		cm = class->methods [i];
		
		/*
		 * Non-virtual method have no place in the vtable.
		 * This also catches static methods (since they are not virtual).
		 */
		if (!(cm->flags & METHOD_ATTRIBUTE_VIRTUAL))
			continue;
		
		/*
		 * If the method is REUSE_SLOT, we must check in the
		 * base class for a method to override.
		 */
		if (!(cm->flags & METHOD_ATTRIBUTE_NEW_SLOT)) {
			int slot = -1;
			for (k = class->parent; k ; k = k->parent) {
				int j;
				for (j = 0; j < k->method.count; ++j) {
					MonoMethod *m1 = k->methods [j];
					if (!(m1->flags & METHOD_ATTRIBUTE_VIRTUAL))
						continue;
					if (!strcmp(cm->name, m1->name) && 
					    mono_metadata_signature_equal (cm->signature, m1->signature)) {
						slot = k->methods [j]->slot;
						g_assert (cm->slot < max_vtsize);
						if (!override_map)
							override_map = mono_g_hash_table_new (NULL, NULL);
						mono_g_hash_table_insert (override_map, m1, cm);
						break;
					}
				}
				if (slot >= 0) 
					break;
			}
			if (slot >= 0)
				cm->slot = slot;
		}

		if (cm->slot < 0)
			cm->slot = cur_slot++;

		if (!(cm->flags & METHOD_ATTRIBUTE_ABSTRACT) && !cm->signature->generic_param_count)
			vtable [cm->slot] = cm;
	}

	/* override non interface methods */
	for (i = 0; i < onum; i++) {
		MonoMethod *decl = overrides [i*2];
		if (!MONO_CLASS_IS_INTERFACE (decl->klass)) {
			g_assert (decl->slot != -1);
			vtable [decl->slot] = overrides [i*2 + 1];
 			overrides [i * 2 + 1]->slot = decl->slot;
			if (!override_map)
				override_map = mono_g_hash_table_new (NULL, NULL);
			mono_g_hash_table_insert (override_map, decl, overrides [i * 2 + 1]);
		}
	}

	/*
	 * If a method occupies more than one place in the vtable, and it is
	 * overriden, then change the other occurances too.
	 */
	if (override_map) {
		for (i = 0; i < max_vtsize; ++i)
			if (vtable [i]) {
				MonoMethod *cm = mono_g_hash_table_lookup (override_map, vtable [i]);
				if (cm)
					vtable [i] = cm;
			}

		mono_g_hash_table_destroy (override_map);
	}

	if (class->generic_inst) {
		MonoClass *gklass = mono_class_from_mono_type (class->generic_inst->generic_type);

		mono_class_init (gklass);
		class->vtable_size = gklass->vtable_size;
	} else       
		class->vtable_size = cur_slot;

	class->vtable = g_malloc0 (sizeof (gpointer) * class->vtable_size);
	memcpy (class->vtable, vtable,  sizeof (gpointer) * class->vtable_size);

	if (mono_print_vtable) {
		int icount = 0;

		for (i = 0; i <= max_iid; i++)
			if (class->interface_offsets [i] != -1)
				icount++;

		printf ("VTable %s.%s (size = %d, interfaces = %d)\n", class->name_space, 
			class->name, class->vtable_size, icount); 

		for (i = 0; i < class->vtable_size; ++i) {
			MonoMethod *cm;
	       
			cm = vtable [i];
			if (cm) {
				printf ("  slot %03d(%03d) %s.%s:%s\n", i, cm->slot,
					cm->klass->name_space, cm->klass->name,
					cm->name);
			}
		}


		if (icount) {
			printf ("Interfaces %s.%s (max_iid = %d)\n", class->name_space, 
				class->name, max_iid);
	
			for (i = 0; i < class->interface_count; i++) {
				ic = class->interfaces [i];
				printf ("  slot %03d(%03d) %s.%s\n",  
					class->interface_offsets [ic->interface_id],
					ic->method.count, ic->name_space, ic->name);
			}

			for (k = class->parent; k ; k = k->parent) {
				for (i = 0; i < k->interface_count; i++) {
					ic = k->interfaces [i]; 
					printf ("  slot %03d(%03d) %s.%s\n", 
						class->interface_offsets [ic->interface_id],
						ic->method.count, ic->name_space, ic->name);
				}
			}
		}
	}
}

/**
 * mono_class_init:
 * @class: the class to initialize
 *
 * compute the instance_size, class_size and other infos that cannot be 
 * computed at mono_class_get() time. Also compute a generic vtable and 
 * the method slot numbers. We use this infos later to create a domain
 * specific vtable.  
 */
void
mono_class_init (MonoClass *class)
{
	int i;
	static MonoMethod *default_ghc = NULL;
	static MonoMethod *default_finalize = NULL;
	static int finalize_slot = -1;
	static int ghc_slot = -1;
 	MonoMethod **overrides;
	int onum = 0;

	g_assert (class);

	if (class->inited)
		return;

	/*g_print ("Init class %s\n", class->name);*/

	/* We do everything inside the lock to prevent races */
	mono_loader_lock ();

	if (class->inited) {
		mono_loader_unlock ();
		/* Somebody might have gotten in before us */
		return;
	}

	if (class->init_pending) {
		/* this indicates a cyclic dependency */
		g_error ("pending init %s.%s\n", class->name_space, class->name);
	}

	class->init_pending = 1;

	mono_stats.initialized_class_count++;

	if (class->generic_inst && !class->generic_inst->is_dynamic) {
		MonoGenericInst *ginst = class->generic_inst;
		MonoClass *gklass;
		GList *list;

		gklass = mono_class_from_mono_type (ginst->generic_type);
		mono_class_init (gklass);

		if (ginst->parent)
			class->parent = mono_class_from_mono_type (ginst->parent);
		else
			class->parent = gklass->parent;

		mono_class_setup_parent (class, class->parent);

		if (MONO_CLASS_IS_INTERFACE (class))
			class->interface_id = mono_get_unique_iid (class);

		class->method = gklass->method;
		class->methods = g_new0 (MonoMethod *, class->method.count);

		for (i = 0; i < class->method.count; i++)
			class->methods [i] = mono_class_inflate_generic_method (
				gklass->methods [i], ginst->context, ginst->klass);

		class->field = gklass->field;
		class->fields = g_new0 (MonoClassField, class->field.count);

		for (i = 0; i < class->field.count; i++) {
			class->fields [i] = gklass->fields [i];
			class->fields [i].generic_type = gklass->fields [i].type;
			class->fields [i].parent = class;
			class->fields [i].type = mono_class_inflate_generic_type (
				class->fields [i].type, ginst->context);
		}

		class->property = gklass->property;
		class->properties = g_new0 (MonoProperty, class->property.count);

		for (i = 0; i < class->property.count; i++) {
			MonoProperty *prop = &class->properties [i];

			*prop = gklass->properties [i];

			if (prop->get)
				prop->get = mono_class_inflate_generic_method (
					prop->get, ginst->context, ginst->klass);
			if (prop->set)
				prop->set = mono_class_inflate_generic_method (
					prop->set, ginst->context, ginst->klass);

			prop->parent = class;
		}

		class->interface_count = gklass->interface_count;
		class->interfaces = g_new0 (MonoClass *, class->interface_count);
		for (i = 0; i < class->interface_count; i++) {
			MonoType *it = &gklass->interfaces [i]->byval_arg;
			MonoType *inflated = mono_class_inflate_generic_type (
				it, ginst->context);
			class->interfaces [i] = mono_class_from_mono_type (inflated);
			mono_class_init (class->interfaces [i]);
		}

		for (list = gklass->nested_classes; list; list = list->next)
			class->nested_classes = g_list_append (
				class->nested_classes, list->data);
	}

	if (class->parent && !class->parent->inited)
		mono_class_init (class->parent);

	/*
	 * Computes the size used by the fields, and their locations
	 */
	if (!class->size_inited)
		class_compute_field_layout (class);

	/* initialize method pointers */
	if (class->rank) {
		MonoMethod *ctor;
		MonoMethodSignature *sig;
		class->method.count = class->rank > 1? 2: 1;
		sig = mono_metadata_signature_alloc (class->image, class->rank);
		sig->ret = &mono_defaults.void_class->byval_arg;
		sig->pinvoke = TRUE;
		for (i = 0; i < class->rank; ++i)
			sig->params [i] = &mono_defaults.int32_class->byval_arg;

		ctor = (MonoMethod *) g_new0 (MonoMethodPInvoke, 1);
		ctor->klass = class;
		ctor->flags = METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_RT_SPECIAL_NAME | METHOD_ATTRIBUTE_SPECIAL_NAME;
		ctor->iflags = METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL;
		ctor->signature = sig;
		ctor->name = ".ctor";
		ctor->slot = -1;
		class->methods = g_new (MonoMethod*, class->method.count);
		class->methods [0] = ctor;
		if (class->rank > 1) {
			sig = mono_metadata_signature_alloc (class->image, class->rank * 2);
			sig->ret = &mono_defaults.void_class->byval_arg;
			sig->pinvoke = TRUE;
			for (i = 0; i < class->rank * 2; ++i)
				sig->params [i] = &mono_defaults.int32_class->byval_arg;

			ctor = (MonoMethod *) g_new0 (MonoMethodPInvoke, 1);
			ctor->klass = class;
			ctor->flags = METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_RT_SPECIAL_NAME | METHOD_ATTRIBUTE_SPECIAL_NAME;
			ctor->iflags = METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL;
			ctor->signature = sig;
			ctor->name = ".ctor";
			ctor->slot = -1;
			class->methods [1] = ctor;
		}
	} else {
		if (!class->generic_inst && !class->methods) {
			class->methods = g_new (MonoMethod*, class->method.count);
			for (i = 0; i < class->method.count; ++i) {
				class->methods [i] = mono_get_method (class->image,
								      MONO_TOKEN_METHOD_DEF | (i + class->method.first + 1), class);
			}
		}
	}

	if (!class->generic_inst) {
		init_properties (class);
		init_events (class);

		i = mono_metadata_nesting_typedef (class->image, class->type_token, 1);
		while (i) {
			MonoClass* nclass;
			guint32 cols [MONO_NESTED_CLASS_SIZE];
			mono_metadata_decode_row (&class->image->tables [MONO_TABLE_NESTEDCLASS], i - 1, cols, MONO_NESTED_CLASS_SIZE);
			nclass = mono_class_create_from_typedef (class->image, MONO_TOKEN_TYPE_DEF | cols [MONO_NESTED_CLASS_NESTED]);
			class->nested_classes = g_list_prepend (class->nested_classes, nclass);

			i = mono_metadata_nesting_typedef (class->image, class->type_token, i + 1);
		}
	}

	mono_class_setup_supertypes (class);

	if (MONO_CLASS_IS_INTERFACE (class)) {
		for (i = 0; i < class->method.count; ++i)
			class->methods [i]->slot = i;
		class->init_pending = 0;
		class->inited = 1;
		/* 
		 * class->interface_offsets is needed for the castclass/isinst code, so
		 * we have to setup them for interfaces, too.
		 */
		setup_interface_offsets (class, 0);
		mono_loader_unlock ();
		return;
	}

 	overrides = mono_class_get_overrides (class->image, class->type_token, &onum);	
	mono_class_setup_vtable (class, overrides, onum);
	g_free (overrides);

	class->inited = 1;
	class->init_pending = 0;

	if (!default_ghc) {
		if (class == mono_defaults.object_class) { 
		       
			for (i = 0; i < class->vtable_size; ++i) {
				MonoMethod *cm = class->vtable [i];
	       
				if (!strcmp (cm->name, "GetHashCode")) {
					ghc_slot = i;
					break;
				}
			}

			g_assert (ghc_slot > 0);

			default_ghc = class->vtable [ghc_slot];
		}
	}
	
	class->ghcimpl = 1;
	if (class->parent) { 

		if (class->vtable [ghc_slot] == default_ghc) {
			class->ghcimpl = 0;
		}
	}

	if (!default_finalize) {
		if (class == mono_defaults.object_class) { 
		       
			for (i = 0; i < class->vtable_size; ++i) {
				MonoMethod *cm = class->vtable [i];
	       
				if (!strcmp (cm->name, "Finalize")) {
					finalize_slot = i;
					break;
				}
			}

			g_assert (finalize_slot > 0);

			default_finalize = class->vtable [finalize_slot];
		}
	}

	/* Object::Finalize should have empty implemenatation */
	class->has_finalize = 0;
	if (class->parent) { 
		if (class->vtable [finalize_slot] != default_finalize)
			class->has_finalize = 1;
	}

	mono_loader_unlock ();

	if (mono_debugger_class_init_func)
		mono_debugger_class_init_func (class);
}


void
mono_class_setup_mono_type (MonoClass *class)
{
	const char *name = class->name;
	const char *nspace = class->name_space;

	if (MONO_CLASS_IS_INTERFACE (class))
		class->interface_id = mono_get_unique_iid (class);

	class->this_arg.byref = 1;
	class->this_arg.data.klass = class;
	class->this_arg.type = MONO_TYPE_CLASS;
	class->byval_arg.data.klass = class;
	class->byval_arg.type = MONO_TYPE_CLASS;

	if (!strcmp (nspace, "System")) {
		if (!strcmp (name, "ValueType")) {
			/*
			 * do not set the valuetype bit for System.ValueType.
			 * class->valuetype = 1;
			 */
		} else if (!strcmp (name, "Enum")) {
			/*
			 * do not set the valuetype bit for System.Enum.
			 * class->valuetype = 1;
			 */
			class->valuetype = 0;
			class->enumtype = 0;
		} else if (!strcmp (name, "Object")) {
			class->this_arg.type = class->byval_arg.type = MONO_TYPE_OBJECT;
		} else if (!strcmp (name, "String")) {
			class->this_arg.type = class->byval_arg.type = MONO_TYPE_STRING;
		} else if (!strcmp (name, "TypedReference")) {
			class->this_arg.type = class->byval_arg.type = MONO_TYPE_TYPEDBYREF;
		}
	}
	
	if (class->valuetype) {
		int t = MONO_TYPE_VALUETYPE;
		if (!strcmp (nspace, "System")) {
			switch (*name) {
			case 'B':
				if (!strcmp (name, "Boolean")) {
					t = MONO_TYPE_BOOLEAN;
				} else if (!strcmp(name, "Byte")) {
					t = MONO_TYPE_U1;
					class->blittable = TRUE;						
				}
				break;
			case 'C':
				if (!strcmp (name, "Char")) {
					t = MONO_TYPE_CHAR;
				}
				break;
			case 'D':
				if (!strcmp (name, "Double")) {
					t = MONO_TYPE_R8;
					class->blittable = TRUE;						
				}
				break;
			case 'I':
				if (!strcmp (name, "Int32")) {
					t = MONO_TYPE_I4;
					class->blittable = TRUE;
				} else if (!strcmp(name, "Int16")) {
					t = MONO_TYPE_I2;
					class->blittable = TRUE;
				} else if (!strcmp(name, "Int64")) {
					t = MONO_TYPE_I8;
					class->blittable = TRUE;
				} else if (!strcmp(name, "IntPtr")) {
					t = MONO_TYPE_I;
					class->blittable = TRUE;
				}
				break;
			case 'S':
				if (!strcmp (name, "Single")) {
					t = MONO_TYPE_R4;
					class->blittable = TRUE;						
				} else if (!strcmp(name, "SByte")) {
					t = MONO_TYPE_I1;
					class->blittable = TRUE;
				}
				break;
			case 'U':
				if (!strcmp (name, "UInt32")) {
					t = MONO_TYPE_U4;
					class->blittable = TRUE;
				} else if (!strcmp(name, "UInt16")) {
					t = MONO_TYPE_U2;
					class->blittable = TRUE;
				} else if (!strcmp(name, "UInt64")) {
					t = MONO_TYPE_U8;
					class->blittable = TRUE;
				} else if (!strcmp(name, "UIntPtr")) {
					t = MONO_TYPE_U;
					class->blittable = TRUE;
				}
				break;
			case 'T':
				if (!strcmp (name, "TypedReference")) {
					t = MONO_TYPE_TYPEDBYREF;
					class->blittable = TRUE;
				}
				break;
			case 'V':
				if (!strcmp (name, "Void")) {
					t = MONO_TYPE_VOID;
				}
				break;
			default:
				break;
			}
		}
		class->this_arg.type = class->byval_arg.type = t;
	}
}

void
mono_class_setup_parent (MonoClass *class, MonoClass *parent)
{
	gboolean system_namespace;

	system_namespace = !strcmp (class->name_space, "System");

	/* if root of the hierarchy */
	if (system_namespace && !strcmp (class->name, "Object")) {
		class->parent = NULL;
		class->instance_size = sizeof (MonoObject);
		return;
	}
	if (!strcmp (class->name, "<Module>")) {
		class->parent = NULL;
		class->instance_size = 0;
		return;
	}

	if (!MONO_CLASS_IS_INTERFACE (class)) {
		class->parent = parent;

		if (!parent)
			g_assert_not_reached (); /* FIXME */

		if (parent->generic_inst && !parent->name) {
			/*
			 * If the parent is a generic instance, we may get
			 * called before it is fully initialized, especially
			 * before it has its name.
			 */
			return;
		}

		class->marshalbyref = parent->marshalbyref;
		class->contextbound  = parent->contextbound;
		class->delegate  = parent->delegate;
		
		if (system_namespace) {
			if (*class->name == 'M' && !strcmp (class->name, "MarshalByRefObject"))
				class->marshalbyref = 1;

			if (*class->name == 'C' && !strcmp (class->name, "ContextBoundObject")) 
				class->contextbound  = 1;

			if (*class->name == 'D' && !strcmp (class->name, "Delegate")) 
				class->delegate  = 1;
		}

		if (class->parent->enumtype || ((strcmp (class->parent->name, "ValueType") == 0) && 
						(strcmp (class->parent->name_space, "System") == 0)))
			class->valuetype = 1;
		if (((strcmp (class->parent->name, "Enum") == 0) && (strcmp (class->parent->name_space, "System") == 0))) {
			class->valuetype = class->enumtype = 1;
		}
		/*class->enumtype = class->parent->enumtype; */
		mono_class_setup_supertypes (class);
	} else {
		class->parent = NULL;
	}

}

void
mono_class_setup_supertypes (MonoClass *class)
{
	MonoClass *k;
	int ms, i;

	if (class->supertypes)
		return;

	class->idepth = 0;
	for (k = class; k ; k = k->parent) {
		class->idepth++;
	}

	ms = MAX (MONO_DEFAULT_SUPERTABLE_SIZE, class->idepth);
	class->supertypes = g_new0 (MonoClass *, ms);

	if (class->parent) {
		for (i = class->idepth, k = class; k ; k = k->parent)
			class->supertypes [--i] = k;
	} else {
		class->supertypes [0] = class;
	}
}	

/**
 * @image: context where the image is created
 * @type_token:  typedef token
 */
static MonoClass *
mono_class_create_from_typedef (MonoImage *image, guint32 type_token)
{
	MonoTableInfo *tt = &image->tables [MONO_TABLE_TYPEDEF];
	MonoClass *class, *parent = NULL;
	guint32 cols [MONO_TYPEDEF_SIZE];
	guint32 cols_next [MONO_TYPEDEF_SIZE];
	guint tidx = mono_metadata_token_index (type_token);
	const char *name, *nspace;
	guint icount = 0; 
	MonoClass **interfaces;

	mono_loader_lock ();

	if ((class = g_hash_table_lookup (image->class_cache, GUINT_TO_POINTER (type_token)))) {
		mono_loader_unlock ();
		return class;
	}

	g_assert (mono_metadata_token_table (type_token) == MONO_TABLE_TYPEDEF);
	
	mono_metadata_decode_row (tt, tidx - 1, cols, MONO_TYPEDEF_SIZE);
	
	name = mono_metadata_string_heap (image, cols [MONO_TYPEDEF_NAME]);
	nspace = mono_metadata_string_heap (image, cols [MONO_TYPEDEF_NAMESPACE]);

	if (cols [MONO_TYPEDEF_EXTENDS])
		parent = mono_class_get (image, mono_metadata_token_from_dor (cols [MONO_TYPEDEF_EXTENDS]));
	interfaces = mono_metadata_interfaces_from_typedef (image, type_token, &icount);

	class = g_malloc0 (sizeof (MonoClass));
			   
	g_hash_table_insert (image->class_cache, GUINT_TO_POINTER (type_token), class);

	class->interfaces = interfaces;
	class->interface_count = icount;

	class->name = name;
	class->name_space = nspace;

	class->image = image;
	class->type_token = type_token;
	class->flags = cols [MONO_TYPEDEF_FLAGS];

	if ((class->flags & TYPE_ATTRIBUTE_STRING_FORMAT_MASK) == TYPE_ATTRIBUTE_UNICODE_CLASS)
		class->unicode = 1;
	/* fixme: maybe we must set this on windows 
	if ((class->flags & TYPE_ATTRIBUTE_STRING_FORMAT_MASK) == TYPE_ATTRIBUTE_AUTO_CLASS)
		class->unicode = 1;
	*/

	class->cast_class = class->element_class = class;

	/*g_print ("Load class %s\n", name);*/

	mono_class_setup_parent (class, parent);

	mono_class_setup_mono_type (class);

	/*
	 * Compute the field and method lists
	 */
	class->field.first  = cols [MONO_TYPEDEF_FIELD_LIST] - 1;
	class->method.first = cols [MONO_TYPEDEF_METHOD_LIST] - 1;

	if (tt->rows > tidx){		
		mono_metadata_decode_row (tt, tidx, cols_next, MONO_TYPEDEF_SIZE);
		class->field.last  = cols_next [MONO_TYPEDEF_FIELD_LIST] - 1;
		class->method.last = cols_next [MONO_TYPEDEF_METHOD_LIST] - 1;
	} else {
		class->field.last  = image->tables [MONO_TABLE_FIELD].rows;
		class->method.last = image->tables [MONO_TABLE_METHOD].rows;
	}

	if (cols [MONO_TYPEDEF_FIELD_LIST] && 
	    cols [MONO_TYPEDEF_FIELD_LIST] <= image->tables [MONO_TABLE_FIELD].rows)
		class->field.count = class->field.last - class->field.first;
	else
		class->field.count = 0;

	if (cols [MONO_TYPEDEF_METHOD_LIST] <= image->tables [MONO_TABLE_METHOD].rows)
		class->method.count = class->method.last - class->method.first;
	else
		class->method.count = 0;

	/* reserve space to store vector pointer in arrays */
	if (!strcmp (nspace, "System") && !strcmp (name, "Array")) {
		class->instance_size += 2 * sizeof (gpointer);
		g_assert (class->field.count == 0);
	}

	if (class->enumtype)
		class_compute_field_layout (class);

	if ((type_token = mono_metadata_nested_in_typedef (image, type_token)))
		class->nested_in = mono_class_create_from_typedef (image, type_token);

	class->gen_params = mono_metadata_load_generic_params (image, class->type_token, &icount);
	class->num_gen_params = icount;

	mono_loader_unlock ();

	return class;
}

MonoClass*
mono_class_create_generic (MonoGenericInst *ginst)
{
	MonoClass *klass, *gklass;

	if (!ginst->klass)
		ginst->klass = g_malloc0 (sizeof (MonoClass));
	klass = ginst->klass;

	gklass = mono_class_from_mono_type (ginst->generic_type);

	klass->nested_in = gklass->nested_in;

	klass->name = gklass->name;
	klass->name_space = gklass->name_space;
	klass->image = gklass->image;
	klass->flags = gklass->flags;

	klass->generic_inst = ginst;

	klass->this_arg.type = klass->byval_arg.type = MONO_TYPE_GENERICINST;
	klass->this_arg.data.generic_inst = klass->byval_arg.data.generic_inst = ginst;
	klass->this_arg.byref = TRUE;

	klass->cast_class = klass->element_class = klass;

	if (ginst->is_dynamic) {
		klass->instance_size = gklass->instance_size;
		klass->class_size = gklass->class_size;
		klass->size_inited = 1;

		klass->valuetype = gklass->valuetype;
	}

	return klass;
}

MonoClass *
mono_class_from_generic_parameter (MonoGenericParam *param, MonoImage *image, gboolean is_mvar)
{
	MonoClass *klass, **ptr;
	int count, pos, i;

	if (param->pklass)
		return param->pklass;

	klass = param->pklass = g_new0 (MonoClass, 1);

	for (count = 0, ptr = param->constraints; ptr && *ptr; ptr++, count++)
		;

	pos = 0;
	if ((count > 0) && !MONO_CLASS_IS_INTERFACE (param->constraints [0])) {
		klass->parent = param->constraints [0];
		pos++;
	}

	if (count - pos > 0) {
		klass->interface_count = count - pos;
		klass->interfaces = g_new0 (MonoClass *, count - pos);
		for (i = pos; i < count; i++)
			klass->interfaces [i - pos] = param->constraints [i];
	}

	g_assert (param->name);

	klass->name = param->name;
	klass->name_space = "";
	klass->image = image;
	klass->cast_class = klass->element_class = klass;
	klass->enum_basetype = &klass->element_class->byval_arg;
	klass->flags = TYPE_ATTRIBUTE_PUBLIC;

	klass->this_arg.type = klass->byval_arg.type = is_mvar ? MONO_TYPE_MVAR : MONO_TYPE_VAR;
	klass->this_arg.data.generic_param = klass->byval_arg.data.generic_param = param;
	klass->this_arg.byref = TRUE;

	mono_class_init (klass);

	return klass;
}

static MonoClass *
my_mono_class_from_generic_parameter (MonoGenericParam *param, gboolean is_mvar)
{
	MonoClass *klass;

	if (param->pklass)
		return param->pklass;

	klass = g_new0 (MonoClass, 1);

	if (param->name)
		klass->name = param->name;
	else
		klass->name = g_strdup_printf (is_mvar ? "!!%d" : "!%d", param->num);
	klass->name_space = "";
	klass->image = mono_defaults.corlib;
	klass->cast_class = klass->element_class = klass;
	klass->enum_basetype = &klass->element_class->byval_arg;
	klass->flags = TYPE_ATTRIBUTE_PUBLIC;

	klass->this_arg.type = klass->byval_arg.type = is_mvar ? MONO_TYPE_MVAR : MONO_TYPE_VAR;
	klass->this_arg.data.generic_param = klass->byval_arg.data.generic_param = param;
	klass->this_arg.byref = TRUE;

	mono_class_init (klass);

	return klass;
}

MonoClass *
mono_ptr_class_get (MonoType *type)
{
	MonoClass *result;
	MonoClass *el_class;
	static GHashTable *ptr_hash = NULL;

	mono_loader_lock ();

	if (!ptr_hash)
		ptr_hash = g_hash_table_new (NULL, NULL);
	el_class = mono_class_from_mono_type (type);
	if ((result = g_hash_table_lookup (ptr_hash, el_class))) {
		mono_loader_unlock ();
		return result;
	}
	result = g_new0 (MonoClass, 1);

	result->parent = NULL; /* no parent for PTR types */
	result->name = "System";
	result->name_space = "MonoPtrFakeClass";
	result->image = el_class->image;
	result->inited = TRUE;
	result->flags = TYPE_ATTRIBUTE_CLASS | (el_class->flags & TYPE_ATTRIBUTE_VISIBILITY_MASK);
	/* Can pointers get boxed? */
	result->instance_size = sizeof (gpointer);
	result->cast_class = result->element_class = el_class;
	result->enum_basetype = &result->element_class->byval_arg;

	result->this_arg.type = result->byval_arg.type = MONO_TYPE_PTR;
	result->this_arg.data.type = result->byval_arg.data.type = result->enum_basetype;
	result->this_arg.byref = TRUE;

	mono_class_setup_supertypes (result);

	g_hash_table_insert (ptr_hash, el_class, result);

	mono_loader_unlock ();

	return result;
}

static MonoClass *
mono_fnptr_class_get (MonoMethodSignature *sig)
{
	MonoClass *result;
	static GHashTable *ptr_hash = NULL;

	mono_loader_lock ();

	if (!ptr_hash)
		ptr_hash = g_hash_table_new (NULL, NULL);
	
	if ((result = g_hash_table_lookup (ptr_hash, sig))) {
		mono_loader_unlock ();
		return result;
	}
	result = g_new0 (MonoClass, 1);

	result->parent = NULL; /* no parent for PTR types */
	result->name = "System";
	result->name_space = "MonoFNPtrFakeClass";
	result->image = NULL; /* need to fix... */
	result->inited = TRUE;
	result->flags = TYPE_ATTRIBUTE_CLASS; // | (el_class->flags & TYPE_ATTRIBUTE_VISIBILITY_MASK);
	/* Can pointers get boxed? */
	result->instance_size = sizeof (gpointer);
	result->cast_class = result->element_class = result;

	result->this_arg.type = result->byval_arg.type = MONO_TYPE_FNPTR;
	result->this_arg.data.method = result->byval_arg.data.method = sig;
	result->this_arg.byref = TRUE;
	result->enum_basetype = &result->element_class->byval_arg;

	mono_class_setup_supertypes (result);

	g_hash_table_insert (ptr_hash, sig, result);

	mono_loader_unlock ();

	return result;
}

MonoClass *
mono_class_from_mono_type (MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_OBJECT:
		return type->data.klass? type->data.klass: mono_defaults.object_class;
	case MONO_TYPE_VOID:
		return type->data.klass? type->data.klass: mono_defaults.void_class;
	case MONO_TYPE_BOOLEAN:
		return type->data.klass? type->data.klass: mono_defaults.boolean_class;
	case MONO_TYPE_CHAR:
		return type->data.klass? type->data.klass: mono_defaults.char_class;
	case MONO_TYPE_I1:
		return type->data.klass? type->data.klass: mono_defaults.sbyte_class;
	case MONO_TYPE_U1:
		return type->data.klass? type->data.klass: mono_defaults.byte_class;
	case MONO_TYPE_I2:
		return type->data.klass? type->data.klass: mono_defaults.int16_class;
	case MONO_TYPE_U2:
		return type->data.klass? type->data.klass: mono_defaults.uint16_class;
	case MONO_TYPE_I4:
		return type->data.klass? type->data.klass: mono_defaults.int32_class;
	case MONO_TYPE_U4:
		return type->data.klass? type->data.klass: mono_defaults.uint32_class;
	case MONO_TYPE_I:
		return type->data.klass? type->data.klass: mono_defaults.int_class;
	case MONO_TYPE_U:
		return type->data.klass? type->data.klass: mono_defaults.uint_class;
	case MONO_TYPE_I8:
		return type->data.klass? type->data.klass: mono_defaults.int64_class;
	case MONO_TYPE_U8:
		return type->data.klass? type->data.klass: mono_defaults.uint64_class;
	case MONO_TYPE_R4:
		return type->data.klass? type->data.klass: mono_defaults.single_class;
	case MONO_TYPE_R8:
		return type->data.klass? type->data.klass: mono_defaults.double_class;
	case MONO_TYPE_STRING:
		return type->data.klass? type->data.klass: mono_defaults.string_class;
	case MONO_TYPE_TYPEDBYREF:
		return type->data.klass? type->data.klass: mono_defaults.typed_reference_class;
	case MONO_TYPE_ARRAY:
		return mono_bounded_array_class_get (type->data.array->eklass, type->data.array->rank, TRUE);
	case MONO_TYPE_PTR:
		return mono_ptr_class_get (type->data.type);
	case MONO_TYPE_FNPTR:
		return mono_fnptr_class_get (type->data.method);
	case MONO_TYPE_SZARRAY:
		return mono_array_class_get (type->data.klass, 1);
	case MONO_TYPE_CLASS:
	case MONO_TYPE_VALUETYPE:
		return type->data.klass;
	case MONO_TYPE_GENERICINST:
		g_assert (type->data.generic_inst->klass);
		return type->data.generic_inst->klass;
	case MONO_TYPE_VAR:
		return my_mono_class_from_generic_parameter (type->data.generic_param, FALSE);
	case MONO_TYPE_MVAR:
		return my_mono_class_from_generic_parameter (type->data.generic_param, TRUE);
	default:
		g_warning ("implement me 0x%02x\n", type->type);
		g_assert_not_reached ();
	}
	
	return NULL;
}

/**
 * @image: context where the image is created
 * @type_spec:  typespec token
 */
static MonoClass *
mono_class_create_from_typespec (MonoImage *image, guint32 type_spec,
				 MonoGenericContext *context)
{
	MonoType *type, *inflated;
	MonoClass *class;

	type = mono_type_create_from_typespec (image, type_spec);

	switch (type->type) {
	case MONO_TYPE_ARRAY:
		class = mono_array_class_get (type->data.array->eklass, type->data.array->rank);
		break;
	case MONO_TYPE_SZARRAY:
		class = mono_array_class_get (type->data.klass, 1);
		break;
	case MONO_TYPE_PTR:
		class = mono_class_from_mono_type (type->data.type);
		break;
	case MONO_TYPE_GENERICINST:
		g_assert (type->data.generic_inst->klass);
		class = type->data.generic_inst->klass;
		break;
	default:
		/* it seems any type can be stored in TypeSpec as well */
		class = mono_class_from_mono_type (type);
		break;
	}

	if (!class || !context)
		return class;

	inflated = mono_class_inflate_generic_type (&class->byval_arg, context);

	return mono_class_from_mono_type (inflated);
}

/**
 * mono_bounded_array_class_get:
 * @element_class: element class 
 * @rank: the dimension of the array class
 * @bounded: whenever the array has non-zero bounds
 *
 * Returns: a class object describing the array with element type @element_type and 
 * dimension @rank. 
 */
MonoClass *
mono_bounded_array_class_get (MonoClass *eclass, guint32 rank, gboolean bounded)
{
	MonoImage *image;
	MonoClass *class;
	MonoClass *parent = NULL;
	GSList *list, *rootlist;
	int nsize;
	char *name;
	gboolean corlib_type = FALSE;

	g_assert (rank <= 255);

	if (rank > 1)
		/* bounded only matters for one-dimensional arrays */
		bounded = FALSE;

	image = eclass->image;

	mono_loader_lock ();

	if ((rootlist = list = g_hash_table_lookup (image->array_cache, eclass))) {
		for (; list; list = list->next) {
			class = list->data;
			if ((class->rank == rank) && (class->byval_arg.type == (bounded ? MONO_TYPE_ARRAY : MONO_TYPE_SZARRAY))) {
				mono_loader_unlock ();
				return class;
			}
		}
	}

	/* for the building corlib use System.Array from it */
	if (image->assembly && image->assembly->dynamic && strcmp (image->assembly_name, "mscorlib") == 0) {
		parent = mono_class_from_name (image, "System", "Array");
		corlib_type = TRUE;
	} else {
		parent = mono_defaults.array_class;
		if (!parent->inited)
			mono_class_init (parent);
	}

	class = g_malloc0 (sizeof (MonoClass));

	class->image = image;
	class->name_space = eclass->name_space;
	nsize = strlen (eclass->name);
	name = g_malloc (nsize + 2 + rank);
	memcpy (name, eclass->name, nsize);
	name [nsize] = '[';
	if (rank > 1)
		memset (name + nsize + 1, ',', rank - 1);
	name [nsize + rank] = ']';
	name [nsize + rank + 1] = 0;
	class->name = name;
	class->type_token = 0;
	/* all arrays are marked serializable and sealed, bug #42779 */
	class->flags = TYPE_ATTRIBUTE_CLASS | TYPE_ATTRIBUTE_SERIALIZABLE | TYPE_ATTRIBUTE_SEALED |
		(eclass->flags & TYPE_ATTRIBUTE_VISIBILITY_MASK);
	class->parent = parent;
	class->instance_size = mono_class_instance_size (class->parent);
	class->class_size = 0;
	mono_class_setup_supertypes (class);

	class->rank = rank;
	
	if (eclass->enumtype)
		class->cast_class = eclass->element_class;
	else
		class->cast_class = eclass;

	class->element_class = eclass;

	if ((rank > 1) || bounded) {
		MonoArrayType *at = g_new0 (MonoArrayType, 1);
		class->byval_arg.type = MONO_TYPE_ARRAY;
		class->byval_arg.data.array = at;
		at->eklass = eclass;
		at->rank = rank;
		/* FIXME: complete.... */
	} else {
		class->byval_arg.type = MONO_TYPE_SZARRAY;
		class->byval_arg.data.klass = eclass;
	}
	class->this_arg = class->byval_arg;
	class->this_arg.byref = 1;
	if (corlib_type) {
		class->inited = 1;
	}

	list = g_slist_append (rootlist, class);
	g_hash_table_insert (image->array_cache, eclass, list);

	mono_loader_unlock ();

	return class;
}

/**
 * mono_array_class_get:
 * @element_class: element class 
 * @rank: the dimension of the array class
 *
 * Returns: a class object describing the array with element type @element_type and 
 * dimension @rank. 
 */
MonoClass *
mono_array_class_get (MonoClass *eclass, guint32 rank)
{
	return mono_bounded_array_class_get (eclass, rank, FALSE);
}

/**
 * mono_class_instance_size:
 * @klass: a class 
 * 
 * Returns: the size of an object instance
 */
gint32
mono_class_instance_size (MonoClass *klass)
{	
	if (!klass->size_inited)
		mono_class_init (klass);

	return klass->instance_size;
}

/**
 * mono_class_min_align:
 * @klass: a class 
 * 
 * Returns: minimm alignment requirements 
 */
gint32
mono_class_min_align (MonoClass *klass)
{	
	if (!klass->size_inited)
		mono_class_init (klass);

	return klass->min_align;
}

/**
 * mono_class_value_size:
 * @klass: a class 
 *
 * This function is used for value types, and return the
 * space and the alignment to store that kind of value object.
 *
 * Returns: the size of a value of kind @klass
 */
gint32
mono_class_value_size      (MonoClass *klass, guint32 *align)
{
	gint32 size;

	/* fixme: check disable, because we still have external revereces to
	 * mscorlib and Dummy Objects 
	 */
	/*g_assert (klass->valuetype);*/

	size = mono_class_instance_size (klass) - sizeof (MonoObject);

	if (align)
		*align = klass->min_align;

	return size;
}

/**
 * mono_class_data_size:
 * @klass: a class 
 * 
 * Returns: the size of the static class data
 */
gint32
mono_class_data_size (MonoClass *klass)
{	
	if (!klass->inited)
		mono_class_init (klass);

	return klass->class_size;
}

/*
 * Auxiliary routine to mono_class_get_field
 *
 * Takes a field index instead of a field token.
 */
static MonoClassField *
mono_class_get_field_idx (MonoClass *class, int idx)
{
	if (class->field.count){
		if ((idx >= class->field.first) && (idx < class->field.last)){
			return &class->fields [idx - class->field.first];
		}
	}

	if (!class->parent)
		return NULL;
	
	return mono_class_get_field_idx (class->parent, idx);
}

/**
 * mono_class_get_field:
 * @class: the class to lookup the field.
 * @field_token: the field token
 *
 * Returns: A MonoClassField representing the type and offset of
 * the field, or a NULL value if the field does not belong to this
 * class.
 */
MonoClassField *
mono_class_get_field (MonoClass *class, guint32 field_token)
{
	int idx = mono_metadata_token_index (field_token);

	g_assert (mono_metadata_token_code (field_token) == MONO_TOKEN_FIELD_DEF);

	return mono_class_get_field_idx (class, idx - 1);
}

MonoClassField *
mono_class_get_field_from_name (MonoClass *klass, const char *name)
{
	int i;

	while (klass) {
		for (i = 0; i < klass->field.count; ++i) {
			if (strcmp (name, klass->fields [i].name) == 0)
				return &klass->fields [i];
		}
		klass = klass->parent;
	}
	return NULL;
}

MonoProperty*
mono_class_get_property_from_name (MonoClass *klass, const char *name)
{
	int i;

	while (klass) {
		for (i = 0; i < klass->property.count; ++i) {
			if (strcmp (name, klass->properties [i].name) == 0)
				return &klass->properties [i];
		}
		klass = klass->parent;
	}
	return NULL;
}

/**
 * mono_class_get:
 * @image: the image where the class resides
 * @type_token: the token for the class
 * @at: an optional pointer to return the array element type
 *
 * Returns: the MonoClass that represents @type_token in @image
 */
MonoClass *
mono_class_get (MonoImage *image, guint32 type_token)
{
	MonoClass *class = NULL;

	if (image->dynamic)
		return mono_lookup_dynamic_token (image, type_token);

	switch (type_token & 0xff000000){
	case MONO_TOKEN_TYPE_DEF:
		class = mono_class_create_from_typedef (image, type_token);
		break;		
	case MONO_TOKEN_TYPE_REF:
		class = mono_class_from_typeref (image, type_token);
		break;
	case MONO_TOKEN_TYPE_SPEC:
		class = mono_class_create_from_typespec (image, type_token, NULL);
		break;
	default:
		g_warning ("unknown token type %x", type_token & 0xff000000);
		g_assert_not_reached ();
	}

	if (!class)
		g_warning ("Could not load class from token 0x%08x in %s", type_token, image->name);

	return class;
}

MonoClass *
mono_class_get_full (MonoImage *image, guint32 type_token, MonoGenericContext *context)
{
	MonoClass *class = mono_class_get (image, type_token);
	MonoType *inflated;

	if (!class || !context)
		return class;

	switch (class->byval_arg.type) {
	case MONO_TYPE_GENERICINST:
		if (!class->generic_inst->is_open)
			return class;
		break;
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		break;
	default:
		return class;
	}

	inflated = inflate_generic_type (&class->byval_arg, context);
	if (!inflated)
		return class;

	return mono_class_from_mono_type (inflated);
}

MonoClass *
mono_class_from_name_case (MonoImage *image, const char* name_space, const char *name)
{
	MonoTableInfo  *t = &image->tables [MONO_TABLE_TYPEDEF];
	guint32 cols [MONO_TYPEDEF_SIZE];
	const char *n;
	const char *nspace;
	guint32 i, visib;

	/* add a cache if needed */
	for (i = 1; i <= t->rows; ++i) {
		mono_metadata_decode_row (t, i - 1, cols, MONO_TYPEDEF_SIZE);
		/* nested types are accessed from the nesting name */
		visib = cols [MONO_TYPEDEF_FLAGS] & TYPE_ATTRIBUTE_VISIBILITY_MASK;
		if (visib > TYPE_ATTRIBUTE_PUBLIC && visib <= TYPE_ATTRIBUTE_NESTED_ASSEMBLY)
			continue;
		n = mono_metadata_string_heap (image, cols [MONO_TYPEDEF_NAME]);
		nspace = mono_metadata_string_heap (image, cols [MONO_TYPEDEF_NAMESPACE]);
		if (g_strcasecmp (n, name) == 0 && g_strcasecmp (nspace, name_space) == 0)
			return mono_class_get (image, MONO_TOKEN_TYPE_DEF | i);
	}
	return NULL;
}

static MonoClass*
return_nested_in (MonoClass *class, char *nested) {
	MonoClass *found;
	char *s = strchr (nested, '/');
	GList *tmp;

	if (s) {
		*s = 0;
		s++;
	}
	for (tmp = class->nested_classes; tmp; tmp = tmp->next) {
		found = tmp->data;
		if (strcmp (found->name, nested) == 0) {
			if (s)
				return return_nested_in (found, s);
			return found;
		}
	}
	return NULL;
}

MonoClass *
mono_class_from_name (MonoImage *image, const char* name_space, const char *name)
{
	GHashTable *nspace_table;
	MonoImage *loaded_image;
	guint32 token = 0;
	MonoClass *class;
	char *nested;
	char buf [1024];

	if ((nested = strchr (name, '/'))) {
		int pos = nested - name;
		int len = strlen (name);
		if (len > 1023)
			return NULL;
		memcpy (buf, name, len + 1);
		buf [pos] = 0;
		nested = buf + pos + 1;
		name = buf;
	}

	mono_loader_lock ();

	nspace_table = g_hash_table_lookup (image->name_cache, name_space);

	if (nspace_table)
		token = GPOINTER_TO_UINT (g_hash_table_lookup (nspace_table, name));

	mono_loader_unlock ();

	if (!token)
		return NULL;

	if (mono_metadata_token_table (token) == MONO_TABLE_EXPORTEDTYPE) {
		MonoTableInfo  *t = &image->tables [MONO_TABLE_EXPORTEDTYPE];
		guint32 cols [MONO_EXP_TYPE_SIZE];
		guint32 idx, impl;

		idx = mono_metadata_token_index (token);

		mono_metadata_decode_row (t, idx - 1, cols, MONO_EXP_TYPE_SIZE);

		impl = cols [MONO_EXP_TYPE_IMPLEMENTATION];
		if ((impl & IMPLEMENTATION_MASK) == IMPLEMENTATION_FILE) {
			loaded_image = mono_assembly_load_module (image->assembly, impl >> IMPLEMENTATION_BITS);
			if (!loaded_image)
				return NULL;
			class = mono_class_from_name (loaded_image, name_space, name);
			if (nested)
				return return_nested_in (class, nested);
			return class;
		} else {
			g_error ("not yet implemented");
		}
	}

	token = MONO_TOKEN_TYPE_DEF | token;

	class = mono_class_get (image, token);
	if (nested)
		return return_nested_in (class, nested);
	return class;
}

gboolean
mono_class_is_subclass_of (MonoClass *klass, MonoClass *klassc, 
			   gboolean check_interfaces)
{
 again:
	if (check_interfaces && MONO_CLASS_IS_INTERFACE (klassc) && !MONO_CLASS_IS_INTERFACE (klass)) {
		if ((klassc->interface_id <= klass->max_interface_id) &&
			(klass->interface_offsets [klassc->interface_id] >= 0))
			return TRUE;
	} else if (check_interfaces && MONO_CLASS_IS_INTERFACE (klassc) && MONO_CLASS_IS_INTERFACE (klass)) {
		int i;

		for (i = 0; i < klass->interface_count; i ++) {
			MonoClass *ic =  klass->interfaces [i];
			if (ic == klassc)
				return TRUE;
		}
	} else {
		if (!MONO_CLASS_IS_INTERFACE (klass) && mono_class_has_parent (klass, klassc))
			return TRUE;
		if (klass->generic_inst) {
			MonoType *parent = klass->generic_inst->parent;
			if (!parent)
				return FALSE;

			if (mono_metadata_type_equal (parent, &klassc->byval_arg))
				return TRUE;
			klass = mono_class_from_mono_type (parent);
			goto again;
		}
	}

	/* 
	 * MS.NET thinks interfaces are a subclass of Object, so we think it as
	 * well.
	 */
	if (klassc == mono_defaults.object_class)
		return TRUE;
	
	return FALSE;
}

gboolean
mono_class_is_assignable_from (MonoClass *klass, MonoClass *oklass)
{
	if (!klass->inited)
		mono_class_init (klass);

	if (!oklass->inited)
		mono_class_init (oklass);

	if (MONO_CLASS_IS_INTERFACE (klass)) {
		if ((klass->interface_id <= oklass->max_interface_id) &&
		    (oklass->interface_offsets [klass->interface_id] != -1))
			return TRUE;
	} else
		if (klass->rank) {
			MonoClass *eclass, *eoclass;

			if (oklass->rank != klass->rank)
				return FALSE;

			/* vectors vs. one dimensional arrays */
			if (oklass->byval_arg.type != klass->byval_arg.type)
				return FALSE;

			eclass = klass->cast_class;
			eoclass = oklass->cast_class;


			/* 
			 * a is b does not imply a[] is b[] when a is a valuetype, and
			 * b is a reference type.
			 */

			if (eoclass->valuetype) {
				if ((eclass == mono_defaults.enum_class) || 
					(eclass == mono_defaults.enum_class->parent) ||
					(eclass == mono_defaults.object_class))
					return FALSE;
			}

			return mono_class_is_assignable_from (klass->cast_class, oklass->cast_class);
		}
	else
		if (klass == mono_defaults.object_class)
			return TRUE;

	return mono_class_has_parent (oklass, klass);
}	

/*
 * mono_class_needs_cctor_run:
 *
 *  Determines whenever the class has a static constructor and whenever it
 * needs to be called when executing CALLER.
 */
gboolean
mono_class_needs_cctor_run (MonoClass *klass, MonoMethod *caller)
{
	int i;
	MonoMethod *method;
	
	for (i = 0; i < klass->method.count; ++i) {
		method = klass->methods [i];
		if ((method->flags & METHOD_ATTRIBUTE_SPECIAL_NAME) && 
		    (strcmp (".cctor", method->name) == 0)) {
			if (caller == method)
				return FALSE;
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * Returns the nnumber of bytes an element of type klass
 * uses when stored into an array.
 */
gint32
mono_class_array_element_size (MonoClass *klass)
{
	int t = klass->byval_arg.type;
	
handle_enum:
	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
		return 1;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
		return 2;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_R4:
		return 4;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_STRING:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY: 
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:   
		return sizeof (gpointer);
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R8:
		return 8;
	case MONO_TYPE_VALUETYPE:
		if (klass->enumtype) {
			t = klass->enum_basetype->type;
			goto handle_enum;
		}
		return mono_class_instance_size (klass) - sizeof (MonoObject);
	default:
		g_error ("unknown type 0x%02x in mono_class_array_element_size", t);
	}
	return -1;
}

/**
 * mono_array_element_size:
 * @ac: pointer to a #MonoArrayClass
 *
 * Returns: the size of single array element.
 */
gint32
mono_array_element_size (MonoClass *ac)
{
	return mono_class_array_element_size (ac->element_class);
}

gpointer
mono_ldtoken (MonoImage *image, guint32 token, MonoClass **handle_class,
	      MonoGenericContext *context)
{
	if (image->dynamic) {
		gpointer obj = mono_lookup_dynamic_token (image, token);

		switch (token & 0xff000000) {
		case MONO_TOKEN_TYPE_DEF:
		case MONO_TOKEN_TYPE_REF:
		case MONO_TOKEN_TYPE_SPEC:
			if (handle_class)
				*handle_class = mono_defaults.typehandle_class;
			return &((MonoClass*)obj)->byval_arg;
		case MONO_TOKEN_METHOD_DEF:
			if (handle_class)
				*handle_class = mono_defaults.methodhandle_class;
			return obj;
		case MONO_TOKEN_FIELD_DEF:
			if (handle_class)
				*handle_class = mono_defaults.fieldhandle_class;
			return obj;
		default:
			g_assert_not_reached ();
		}
	}

	switch (token & 0xff000000) {
	case MONO_TOKEN_TYPE_DEF:
	case MONO_TOKEN_TYPE_REF: {
		MonoClass *class;
		if (handle_class)
			*handle_class = mono_defaults.typehandle_class;
		class = mono_class_get_full (image, token, context);
		mono_class_init (class);
		/* We return a MonoType* as handle */
		return &class->byval_arg;
	}
	case MONO_TOKEN_TYPE_SPEC: {
		MonoClass *class;
		if (handle_class)
			*handle_class = mono_defaults.typehandle_class;
		class = mono_class_create_from_typespec (image, token, context);
		mono_class_init (class);
		return &class->byval_arg;
	}
	case MONO_TOKEN_FIELD_DEF: {
		MonoClass *class;
		guint32 type = mono_metadata_typedef_from_field (image, mono_metadata_token_index (token));
		class = mono_class_get_full (image, MONO_TOKEN_TYPE_DEF | type, context);
		mono_class_init (class);
		if (handle_class)
			*handle_class = mono_defaults.fieldhandle_class;
		return mono_class_get_field (class, token);
	}
	case MONO_TOKEN_METHOD_DEF: {
		MonoMethod *meth;
		meth = mono_get_method_full (image, token, NULL, context);
		if (handle_class)
			*handle_class = mono_defaults.methodhandle_class;
		return meth;
	}
	case MONO_TOKEN_MEMBER_REF: {
		guint32 cols [MONO_MEMBERREF_SIZE];
		const char *sig;
		mono_metadata_decode_row (&image->tables [MONO_TABLE_MEMBERREF], mono_metadata_token_index (token) - 1, cols, MONO_MEMBERREF_SIZE);
		sig = mono_metadata_blob_heap (image, cols [MONO_MEMBERREF_SIGNATURE]);
		mono_metadata_decode_blob_size (sig, &sig);
		if (*sig == 0x6) { /* it's a field */
			MonoClass *klass;
			MonoClassField *field;
			field = mono_field_from_token (image, token, &klass, context);
			if (handle_class)
				*handle_class = mono_defaults.fieldhandle_class;
			return field;
		} else {
			MonoMethod *meth;
			meth = mono_get_method_full (image, token, NULL, context);
			if (handle_class)
				*handle_class = mono_defaults.methodhandle_class;
			return meth;
		}
	}
	default:
		g_warning ("Unknown token 0x%08x in ldtoken", token);
		break;
	}
	return NULL;
}

/**
 * This function might need to call runtime functions so it can't be part
 * of the metadata library.
 */
static MonoLookupDynamicToken lookup_dynamic = NULL;

void
mono_install_lookup_dynamic_token (MonoLookupDynamicToken func)
{
	lookup_dynamic = func;
}

gpointer
mono_lookup_dynamic_token (MonoImage *image, guint32 token)
{
	return lookup_dynamic (image, token);
}



