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

/*
 * Uncomment this to enable GC aware auto layout: in this mode, reference
 * fields are grouped together inside objects, increasing collector 
 * performance.
 * Requires that all classes whose layout is known to the runtime be annotated
 * with [StructLayout (LayoutKind.Sequential)]
 */
//#define GC_AWARE_AUTO_LAYOUT

#define CSIZE(x) (sizeof (x) / 4)

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
	MonoImageOpenStatus status;

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

	mono_image_load_references (image, &status);
	references = image->references;
	if (!references ||  !references [idx-1]) {
		/* 
		 * detected a reference to mscorlib, we simply return a reference to a dummy 
		 * until we have a better solution.
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
		guint32 constant_cols [MONO_CONSTANT_SIZE];
		guint32 cindex;
		int idx = class->field.first + i;
		
		mono_metadata_decode_row (t, idx, cols, CSIZE (cols));
		/* The name is needed for fieldrefs */
		class->fields [i].name = mono_metadata_string_heap (m, cols [MONO_FIELD_NAME]);
		sig = mono_metadata_blob_heap (m, cols [MONO_FIELD_SIGNATURE]);
		mono_metadata_decode_value (sig, &sig);
		/* FIELD signature == 0x06 */
		g_assert (*sig == 0x06);
		class->fields [i].type = mono_metadata_parse_field_type (
			m, cols [MONO_FIELD_FLAGS], sig + 1, &sig);

		class->fields [i].parent = class;

		if (!(class->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC)) {
			if (class->fields [i].type->byref) {
				blittable = FALSE;
			} else {
				MonoClass *field_class = mono_class_from_mono_type (class->fields [i].type);
				if (!field_class || !field_class->blittable)
					blittable = FALSE;
			}
		}
		if (layout == TYPE_ATTRIBUTE_EXPLICIT_LAYOUT) {
			mono_metadata_field_info (m, idx, &class->fields [i].offset, NULL, NULL);
			if (class->fields [i].offset == (guint32)-1)
				g_warning ("%s not initialized correctly (missing field layout info for %s)", class->name, class->fields [i].name);
		}

		if (cols [MONO_FIELD_FLAGS] & FIELD_ATTRIBUTE_HAS_FIELD_RVA) {
			mono_metadata_field_info (m, idx, NULL, &rva, NULL);
			if (!rva)
				g_warning ("field %s in %s should have RVA data, but hasn't", class->fields [i].name, class->name);
			class->fields [i].data = mono_cli_rva_map (class->image->image_info, rva);
		}

		if (class->enumtype && !(cols [MONO_FIELD_FLAGS] & FIELD_ATTRIBUTE_STATIC)) {
			class->enum_basetype = class->fields [i].type;
			class->cast_class = class->element_class = mono_class_from_mono_type (class->enum_basetype);
			blittable = class->element_class->blittable;
		}

		if ((class->fields [i].type->attrs & FIELD_ATTRIBUTE_HAS_DEFAULT) &&
			(class->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC)) {
			cindex = mono_metadata_get_constant_index (class->image, MONO_TOKEN_FIELD_DEF | (class->field.first + i + 1));
			if (!cindex) {
				g_warning ("constant for field %s:%s not found", class->name, class->fields [i].name);
				continue;
			}
			mono_metadata_decode_row (&class->image->tables [MONO_TABLE_CONSTANT], cindex - 1, constant_cols, MONO_CONSTANT_SIZE);
			class->fields [i].def_value = g_new0 (MonoConstant, 1);
			class->fields [i].def_value->type = constant_cols [MONO_CONSTANT_TYPE];
			class->fields [i].def_value->value = (gpointer)mono_metadata_blob_heap (class->image, constant_cols [MONO_CONSTANT_VALUE]);
		}
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

	mono_class_layout_fields (class);
}

void
mono_class_layout_fields (MonoClass *class)
{
	int i;
	const int top = class->field.count;
	guint32 layout = class->flags & TYPE_ATTRIBUTE_LAYOUT_MASK;
	guint32 pass, passes, real_size;

	/*
	 * Compute field layout and total size (not considering static fields)
	 */

	switch (layout) {
	case TYPE_ATTRIBUTE_AUTO_LAYOUT:
	case TYPE_ATTRIBUTE_SEQUENTIAL_LAYOUT:

#ifdef GC_AWARE_AUTO_LAYOUT
		passes = 2;
#else
		passes = 1;
#endif
		if (layout != TYPE_ATTRIBUTE_AUTO_LAYOUT)
			passes = 1;

		for (pass = 0; pass < passes; ++pass) {
			if (class->parent)
				real_size = class->parent->instance_size;
			else
				real_size = sizeof (MonoObject);

			for (i = 0; i < top; i++){
				int size, align;

				if (class->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC)
					continue;

#ifdef GC_AWARE_AUTO_LAYOUT
				/* FIXME: Fix mono_marshal_load_type_info () too */
				if (layout == TYPE_ATTRIBUTE_AUTO_LAYOUT) {
					/* 
					 * We process fields with reference type in the first pass,
					 * and fields with non-reference type in the second pass.
					 * We use IS_POINTER instead of IS_REFERENCE because in
					 * some internal structures, we store GC_MALLOCed memory
					 * in IntPtr fields...
					 */
					if (MONO_TYPE_IS_POINTER (class->fields [i].type)) {
						if (pass == 1)
							continue;
					} else {
						if (pass == 0)
							continue;
					}
				}
#endif

				size = mono_type_size (class->fields [i].type, &align);
			
				/* FIXME (LAMESPEC): should we also change the min alignment according to pack? */
				align = class->packing_size ? MIN (class->packing_size, align): align;
				class->min_align = MAX (align, class->min_align);
				class->fields [i].offset = real_size;
				class->fields [i].offset += align - 1;
				class->fields [i].offset &= ~(align - 1);
				real_size = class->fields [i].offset + size;
			}

			class->instance_size = MAX (real_size, class->instance_size);
       
			if (class->instance_size & (class->min_align - 1)) {
				class->instance_size += class->min_align - 1;
				class->instance_size &= ~(class->min_align - 1);
			}
		}
		break;
	case TYPE_ATTRIBUTE_EXPLICIT_LAYOUT:
		for (i = 0; i < top; i++) {
			int size, align;

			/*
			 * There must be info about all the fields in a type if it
			 * uses explicit layout.
			 */

			if (class->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			size = mono_type_size (class->fields [i].type, &align);
			
			/*
			 * When we get here, class->fields [i].offset is already set by the
			 * loader (for either runtime fields or fields loaded from metadata).
			 * The offset is from the start of the object: this works for both
			 * classes and valuetypes.
			 */
			class->fields [i].offset += sizeof (MonoObject);

			/*
			 * Calc max size.
			 */
			real_size = MAX (real_size, size + class->fields [i].offset);
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
			
		if (!(class->fields [i].type->attrs & FIELD_ATTRIBUTE_STATIC))
			continue;
			
		size = mono_type_size (class->fields [i].type, &align);
		class->fields [i].offset = class->class_size;
		class->fields [i].offset += align - 1;
		class->fields [i].offset &= ~(align - 1);
		class->class_size = class->fields [i].offset + size;
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
	
	g_assert (class->flags & TYPE_ATTRIBUTE_INTERFACE);

	if (!iid_hash)
		iid_hash = g_hash_table_new (g_str_hash, g_str_equal);

	str = g_strdup_printf ("%s|%s.%s\n", class->image->name, class->name_space, class->name);

	if (g_hash_table_lookup_extended (iid_hash, str, NULL, &value)) {
		g_free (str);
		return (guint)value;
	} else {
		g_hash_table_insert (iid_hash, str, (gpointer)iid);
		++iid;
	}

	return iid - 1;
}

static void
collect_implemented_interfaces_aux (MonoClass *klass, GPtrArray *res)
{
	int i;
	MonoClass *ic;
	
	for (i = 0; i < klass->interface_count; i++) {
		ic = klass->interfaces [i];

		g_ptr_array_add (res, ic);

		collect_implemented_interfaces_aux (ic, res);
	}
}

static GPtrArray*
collect_implemented_interfaces (MonoClass *klass)
{
	GPtrArray *res = g_ptr_array_new ();

	collect_implemented_interfaces_aux (klass, res);
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

	if (class->flags & TYPE_ATTRIBUTE_INTERFACE) {
		if (max_iid < class->interface_id)
			max_iid = class->interface_id;
	}
	class->max_interface_id = max_iid;
	/* compute vtable offset for interfaces */
	class->interface_offsets = g_malloc (sizeof (gpointer) * (max_iid + 1));

	for (i = 0; i <= max_iid; i++)
		class->interface_offsets [i] = -1;

	ifaces = collect_implemented_interfaces (class);
	for (i = 0; i < ifaces->len; ++i) {
		ic = g_ptr_array_index (ifaces, i);
		class->interface_offsets [ic->interface_id] = cur_slot;
		cur_slot += ic->method.count;
	}
	g_ptr_array_free (ifaces, TRUE);

	for (k = class->parent; k ; k = k->parent) {
		ifaces = collect_implemented_interfaces (k);
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
	return cur_slot;
}

void
mono_class_setup_vtable (MonoClass *class, MonoMethod **overrides, int onum)
{
	MonoClass *k, *ic;
	MonoMethod **vtable;
	int i, max_vtsize = 0, max_iid, cur_slot = 0;
	GPtrArray *ifaces;
	MonoGHashTable *override_map;

	/* setup_vtable() must be called only once on the type */
	if (class->interface_offsets) {
		g_warning ("vtable already computed in %s.%s", class->name_space, class->name);
		return;
	}

	ifaces = collect_implemented_interfaces (class);
	for (i = 0; i < ifaces->len; i++) {
		MonoClass *ic = g_ptr_array_index (ifaces, i);
		max_vtsize += ic->method.count;
	}
	g_ptr_array_free (ifaces, TRUE);
	
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

	override_map = mono_g_hash_table_new (NULL, NULL);

	/* override interface methods */
	for (i = 0; i < onum; i++) {
		MonoMethod *decl = overrides [i*2];
		if (decl->klass->flags & TYPE_ATTRIBUTE_INTERFACE) {
			int dslot;
			g_assert (decl->slot != -1);
			dslot = decl->slot + class->interface_offsets [decl->klass->interface_id];
			vtable [dslot] = overrides [i*2 + 1];
			vtable [dslot]->slot = dslot;
			mono_g_hash_table_insert (override_map, overrides [i * 2], overrides [i * 2 + 1]);
		}
	}

	for (k = class; k ; k = k->parent) {
		ifaces = collect_implemented_interfaces (k);
		for (i = 0; i < ifaces->len; i++) {
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
		g_ptr_array_free (ifaces, TRUE);
	} 

	for (i = 0; i < class->method.count; ++i) {
		MonoMethod *cm;
	       
		cm = class->methods [i];

		if (!(cm->flags & METHOD_ATTRIBUTE_NEW_SLOT) && (cm->flags & METHOD_ATTRIBUTE_VIRTUAL)) {
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

		if (!(cm->flags & METHOD_ATTRIBUTE_ABSTRACT))
			vtable [cm->slot] = cm;
	}

	/* override non interface methods */
	for (i = 0; i < onum; i++) {
		MonoMethod *decl = overrides [i*2];
		if (!(decl->klass->flags & TYPE_ATTRIBUTE_INTERFACE)) {
			g_assert (decl->slot != -1);
			vtable [decl->slot] = overrides [i*2 + 1];
 			overrides [i * 2 + 1]->slot = decl->slot;
			mono_g_hash_table_insert (override_map, decl, overrides [i * 2 + 1]);
		}
	}

	/*
	 * If a method occupies more than one place in the vtable, and it is
	 * overriden, then change the other occurances too.
	 */
	for (i = 0; i < max_vtsize; ++i)
		if (vtable [i]) {
			MonoMethod *cm = mono_g_hash_table_lookup (override_map, vtable [i]);
			if (cm)
				vtable [i] = cm;
		}
	mono_g_hash_table_destroy (override_map);

       
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

	if (class->init_pending) {
		/* this indicates a cyclic dependency */
		g_error ("pending init %s.%s\n", class->name_space, class->name);
	}

	class->init_pending = 1;

	mono_stats.initialized_class_count++;

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
			class->methods [1] = ctor;
		}
	} else {
		class->methods = g_new (MonoMethod*, class->method.count);
		for (i = 0; i < class->method.count; ++i) {
			class->methods [i] = mono_get_method (class->image,
				MONO_TOKEN_METHOD_DEF | (i + class->method.first + 1), class);
		}
	}

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

	mono_class_setup_supertypes (class);

	if (class->flags & TYPE_ATTRIBUTE_INTERFACE) {
		for (i = 0; i < class->method.count; ++i)
			class->methods [i]->slot = i;
		class->init_pending = 0;
		class->inited = 1;
		/* 
		 * class->interface_offsets is needed for the castclass/isinst code, so
		 * we have to setup them for interfaces, too.
		 */
		setup_interface_offsets (class, 0);
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

	if (mono_debugger_class_init_func)
		mono_debugger_class_init_func (class);
}


void
mono_class_setup_mono_type (MonoClass *class)
{
	const char *name = class->name;
	const char *nspace = class->name_space;

	if (class->flags & TYPE_ATTRIBUTE_INTERFACE)
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

	if (!(class->flags & TYPE_ATTRIBUTE_INTERFACE)) {
		class->parent = parent;

		if (!parent)
			g_assert_not_reached (); /* FIXME */

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
		class->parent->subclasses = g_list_prepend (class->parent->subclasses, class);
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

	if ((class = g_hash_table_lookup (image->class_cache, GUINT_TO_POINTER (type_token)))) 
		return class;

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

	/*g_print ("Init class %s\n", name);*/

	mono_class_setup_parent (class, parent);

	mono_class_setup_mono_type (class);

	/*
	 * Compute the field and method lists
	 */
	class->field.first  = cols [MONO_TYPEDEF_FIELD_LIST] - 1;
	class->method.first = cols [MONO_TYPEDEF_METHOD_LIST] - 1;

	if (tt->rows > tidx){		
		mono_metadata_decode_row (tt, tidx, cols_next, CSIZE (cols_next));
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

	return class;
}

MonoClass *
mono_ptr_class_get (MonoType *type)
{
	MonoClass *result;
	MonoClass *el_class;
	static GHashTable *ptr_hash = NULL;

	if (!ptr_hash)
		ptr_hash = g_hash_table_new (g_direct_hash, g_direct_equal);
	el_class = mono_class_from_mono_type (type);
	if ((result = g_hash_table_lookup (ptr_hash, el_class)))
		return result;
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

	return result;
}

static MonoClass *
mono_fnptr_class_get (MonoMethodSignature *sig)
{
	MonoClass *result;
	static GHashTable *ptr_hash = NULL;

	if (!ptr_hash)
		ptr_hash = g_hash_table_new (g_direct_hash, g_direct_equal);
	
	if ((result = g_hash_table_lookup (ptr_hash, sig)))
		return result;
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

	return result;
}

MonoClass *
mono_class_from_mono_type (MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_OBJECT:
		return mono_defaults.object_class;
	case MONO_TYPE_VOID:
		return mono_defaults.void_class;
	case MONO_TYPE_BOOLEAN:
		return mono_defaults.boolean_class;
	case MONO_TYPE_CHAR:
		return mono_defaults.char_class;
	case MONO_TYPE_I1:
		return mono_defaults.sbyte_class;
	case MONO_TYPE_U1:
		return mono_defaults.byte_class;
	case MONO_TYPE_I2:
		return mono_defaults.int16_class;
	case MONO_TYPE_U2:
		return mono_defaults.uint16_class;
	case MONO_TYPE_I4:
		return mono_defaults.int32_class;
	case MONO_TYPE_U4:
		return mono_defaults.uint32_class;
	case MONO_TYPE_I:
		return mono_defaults.int_class;
	case MONO_TYPE_U:
		return mono_defaults.uint_class;
	case MONO_TYPE_I8:
		return mono_defaults.int64_class;
	case MONO_TYPE_U8:
		return mono_defaults.uint64_class;
	case MONO_TYPE_R4:
		return mono_defaults.single_class;
	case MONO_TYPE_R8:
		return mono_defaults.double_class;
	case MONO_TYPE_STRING:
		return mono_defaults.string_class;
	case MONO_TYPE_TYPEDBYREF:
		return mono_defaults.typed_reference_class;
	case MONO_TYPE_ARRAY:
		return mono_array_class_get (type->data.array->type, type->data.array->rank);
	case MONO_TYPE_PTR:
		return mono_ptr_class_get (type->data.type);
	case MONO_TYPE_FNPTR:
		return mono_fnptr_class_get (type->data.method);
	case MONO_TYPE_SZARRAY:
		return mono_array_class_get (type->data.type, 1);
	case MONO_TYPE_CLASS:
	case MONO_TYPE_VALUETYPE:
		return type->data.klass;
		
	case MONO_TYPE_GENERICINST:
		g_warning ("mono_class_from_type: implement me MONO_TYPE_GENERICINST");
		g_assert_not_reached ();
		
	case MONO_TYPE_VAR:
		g_warning ("mono_class_from_type: implement me MONO_TYPE_VAR");
		g_assert_not_reached ();

	case MONO_TYPE_MVAR:
		g_warning ("mono_class_from_type: implement me MONO_TYPE_MVAR");
		g_assert_not_reached ();
		
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
mono_class_create_from_typespec (MonoImage *image, guint32 type_spec)
{
	MonoType *type;
	MonoClass *class;

	type = mono_type_create_from_typespec (image, type_spec);

	switch (type->type) {
	case MONO_TYPE_ARRAY:
		class = mono_array_class_get (type->data.array->type, type->data.array->rank);
		break;
	case MONO_TYPE_SZARRAY:
		class = mono_array_class_get (type->data.type, 1);
		break;
	case MONO_TYPE_PTR:
		class = mono_class_from_mono_type (type->data.type);
		break;
	default:
		/* it seems any type can be stored in TypeSpec as well */
		class = mono_class_from_mono_type (type);
		break;
	}

	mono_metadata_free_type (type);
	
	return class;
}

/**
 * mono_array_class_get:
 * @element_type: element type 
 * @rank: the dimension of the array class
 *
 * Returns: a class object describing the array with element type @element_type and 
 * dimension @rank. 
 */
MonoClass *
mono_array_class_get (MonoType *element_type, guint32 rank)
{
	MonoClass *eclass;
	MonoImage *image;
	MonoClass *class;
	MonoClass *parent = NULL;
	GSList *list, *rootlist;
	int nsize;
	char *name;

	eclass = mono_class_from_mono_type (element_type);
	g_assert (rank <= 255);

	parent = mono_defaults.array_class;

	if (!parent->inited)
		mono_class_init (parent);

	image = eclass->image;

	if ((rootlist = list = g_hash_table_lookup (image->array_cache, &eclass->byval_arg))) {
		for (; list; list = list->next) {
			class = list->data;
			if (class->rank == rank)
				return class;
		}
	}

	class = g_malloc0 (sizeof (MonoClass) + parent->vtable_size * sizeof (gpointer));

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
	class->vtable_size = parent->vtable_size;
	class->parent->subclasses = g_list_prepend (class->parent->subclasses, class);
	mono_class_setup_supertypes (class);

	class->rank = rank;
	
	if (eclass->enumtype)
		class->cast_class = eclass->element_class;
	else
		class->cast_class = eclass;

	class->element_class = eclass;

	if (rank > 1) {
		MonoArrayType *at = g_new0 (MonoArrayType, 1);
		class->byval_arg.type = MONO_TYPE_ARRAY;
		class->byval_arg.data.array = at;
		at->type = &eclass->byval_arg;
		at->rank = rank;
		/* FIXME: complete.... */
	} else {
		/* FIXME: this is not correct. the lbound could be >0 */
		class->byval_arg.type = MONO_TYPE_SZARRAY;
		class->byval_arg.data.type = &eclass->byval_arg;
	}
	class->this_arg = class->byval_arg;
	class->this_arg.byref = 1;

	list = g_slist_append (rootlist, class);
	g_hash_table_insert (image->array_cache, &class->element_class->byval_arg, list);
	return class;
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
	MonoClass *class;

	if (image->assembly->dynamic)
		return mono_lookup_dynamic_token (image, type_token);

	switch (type_token & 0xff000000){
	case MONO_TOKEN_TYPE_DEF:
		class = mono_class_create_from_typedef (image, type_token);
		break;		
	case MONO_TOKEN_TYPE_REF:
		class = mono_class_from_typeref (image, type_token);
		break;
	case MONO_TOKEN_TYPE_SPEC:
		class = mono_class_create_from_typespec (image, type_token);
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

static MonoImage*
load_file_for_image (MonoImage *image, int fileidx)
{
	char *base_dir, *name;
	MonoImage *res;
	MonoTableInfo  *t = &image->tables [MONO_TABLE_FILE];
	const char *fname;
	guint32 fname_id;

	if (fileidx < 1 || fileidx > t->rows)
		return NULL;
	fname_id = mono_metadata_decode_row_col (t, fileidx - 1, MONO_FILE_NAME);
	fname = mono_metadata_string_heap (image, fname_id);
	base_dir = g_path_get_dirname (image->name);
	name = g_build_filename (base_dir, fname, NULL);
	res = mono_image_open (name, NULL);
	if (res) {
		int i;
		t = &res->tables [MONO_TABLE_MODULEREF];
		//g_print ("loaded file %s from %s (%p)\n", name, image->name, image->assembly);
		res->assembly = image->assembly;
		for (i = 0; i < t->rows; ++i) {
			if (res->modules [i] && !res->modules [i]->assembly)
				res->modules [i]->assembly = image->assembly;
		}
		mono_image_load_references (image, NULL);
	}
	g_free (name);
	g_free (base_dir);
	return res;
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
	guint32 token;
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

	nspace_table = g_hash_table_lookup (image->name_cache, name_space);
	
	if (!nspace_table || !(token = GPOINTER_TO_UINT (g_hash_table_lookup (nspace_table, name)))) {
		MonoTableInfo  *t = &image->tables [MONO_TABLE_EXPORTEDTYPE];
		guint32 cols [MONO_EXP_TYPE_SIZE];
		int i;

		for (i = 0; i < t->rows; ++i) {
			const char *ename, *enspace;
			mono_metadata_decode_row (t, i, cols, MONO_EXP_TYPE_SIZE);
			ename = mono_metadata_string_heap (image, cols [MONO_EXP_TYPE_NAME]);
			enspace = mono_metadata_string_heap (image, cols [MONO_EXP_TYPE_NAMESPACE]);

			if (strcmp (name, ename) == 0 && strcmp (name_space, enspace) == 0) {
				guint32 impl = cols [MONO_EXP_TYPE_IMPLEMENTATION];
				if ((impl & IMPLEMENTATION_MASK) == IMPLEMENTATION_FILE) {
					loaded_image = load_file_for_image (image, impl >> IMPLEMENTATION_BITS);
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
		}
		/*g_warning ("token not found for %s.%s in image %s", name_space, name, image->name);*/
		return NULL;
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
	if (check_interfaces && (klassc->flags & TYPE_ATTRIBUTE_INTERFACE) && !(klass->flags & TYPE_ATTRIBUTE_INTERFACE)) {
		if ((klassc->interface_id <= klass->max_interface_id) &&
			(klass->interface_offsets [klassc->interface_id] >= 0))
			return TRUE;
	} else if (check_interfaces && (klassc->flags & TYPE_ATTRIBUTE_INTERFACE) && (klass->flags & TYPE_ATTRIBUTE_INTERFACE)) {
		int i;

		for (i = 0; i < klass->interface_count; i ++) {
			MonoClass *ic =  klass->interfaces [i];
			if (ic == klassc)
				return TRUE;
		}
	} else {
		if (!(klass->flags & TYPE_ATTRIBUTE_INTERFACE) && mono_class_has_parent (klass, klassc))
			return TRUE;
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
mono_ldtoken (MonoImage *image, guint32 token, MonoClass **handle_class)
{
	if (image->assembly->dynamic) {
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
		class = mono_class_get (image, token);
		mono_class_init (class);
		/* We return a MonoType* as handle */
		return &class->byval_arg;
	}
	case MONO_TOKEN_TYPE_SPEC: {
		MonoClass *class;
		if (handle_class)
			*handle_class = mono_defaults.typehandle_class;
		class = mono_class_create_from_typespec (image, token);
		mono_class_init (class);
		return &class->byval_arg;
	}
	case MONO_TOKEN_FIELD_DEF: {
		MonoClass *class;
		guint32 type = mono_metadata_typedef_from_field (image, mono_metadata_token_index (token));
		class = mono_class_get (image, MONO_TOKEN_TYPE_DEF | type);
		mono_class_init (class);
		if (handle_class)
				*handle_class = mono_defaults.fieldhandle_class;
		return mono_class_get_field (class, token);
	}
	case MONO_TOKEN_METHOD_DEF:
	case MONO_TOKEN_MEMBER_REF:
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
