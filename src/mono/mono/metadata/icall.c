/*
 * icall.c:
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Paolo Molaro (lupus@ximian.com)
 *	 Patrik Torstensson (patrik.torstensson@labs2.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <unistd.h>
#if defined (PLATFORM_WIN32)
#include <stdlib.h>
#endif

#include <mono/metadata/object.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/threadpool.h>
#include <mono/metadata/monitor.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/file-io.h>
#include <mono/metadata/socket-io.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/unicode.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/rand.h>
#include <mono/metadata/sysmath.h>
#include <mono/metadata/string-icalls.h>
#include <mono/metadata/mono-debug-debugger.h>
#include <mono/metadata/process.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/locales.h>
#include <mono/metadata/filewatcher.h>
#include <mono/metadata/char-conversions.h>
#include <mono/metadata/security.h>
#include <mono/metadata/mono-config.h>
#include <mono/io-layer/io-layer.h>
#include <mono/utils/strtod.h>
#include <mono/utils/monobitset.h>

#if defined (PLATFORM_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif
#include "decimal.h"

static MonoReflectionAssembly* ves_icall_System_Reflection_Assembly_GetCallingAssembly (void);


/*
 * We expect a pointer to a char, not a string
 */
static double
mono_double_ParseImpl (char *ptr)
{
	gchar *endptr = NULL;
	gdouble result = 0.0;

	MONO_ARCH_SAVE_REGS;

	if (*ptr)
		result = bsd_strtod (ptr, &endptr);

	if (!*ptr || (endptr && *endptr))
		mono_raise_exception (mono_exception_from_name (mono_defaults.corlib,
								"System",
								"FormatException"));
	
	return result;
}

static void
ves_icall_System_Double_AssertEndianity (double *value)
{
	MONO_ARCH_SAVE_REGS;

	MONO_DOUBLE_ASSERT_ENDIANITY (value);
}

static MonoObject *
ves_icall_System_Array_GetValueImpl (MonoObject *this, guint32 pos)
{
	MonoClass *ac;
	MonoArray *ao;
	gint32 esize;
	gpointer *ea;

	MONO_ARCH_SAVE_REGS;

	ao = (MonoArray *)this;
	ac = (MonoClass *)ao->obj.vtable->klass;

	esize = mono_array_element_size (ac);
	ea = (gpointer*)((char*)ao->vector + (pos * esize));

	if (ac->element_class->valuetype)
		return mono_value_box (this->vtable->domain, ac->element_class, ea);
	else
		return *ea;
}

static MonoObject *
ves_icall_System_Array_GetValue (MonoObject *this, MonoObject *idxs)
{
	MonoClass *ac, *ic;
	MonoArray *ao, *io;
	gint32 i, pos, *ind;

	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (idxs);

	io = (MonoArray *)idxs;
	ic = (MonoClass *)io->obj.vtable->klass;
	
	ao = (MonoArray *)this;
	ac = (MonoClass *)ao->obj.vtable->klass;

	g_assert (ic->rank == 1);
	if (io->bounds != NULL || io->max_length !=  ac->rank)
		mono_raise_exception (mono_get_exception_argument (NULL, NULL));

	ind = (guint32 *)io->vector;

	if (ao->bounds == NULL) {
		if (*ind < 0 || *ind >= ao->max_length)
			mono_raise_exception (mono_get_exception_index_out_of_range ());

		return ves_icall_System_Array_GetValueImpl (this, *ind);
	}
	
	for (i = 0; i < ac->rank; i++)
		if ((ind [i] < ao->bounds [i].lower_bound) ||
		    (ind [i] >= ao->bounds [i].length + ao->bounds [i].lower_bound))
			mono_raise_exception (mono_get_exception_index_out_of_range ());

	pos = ind [0] - ao->bounds [0].lower_bound;
	for (i = 1; i < ac->rank; i++)
		pos = pos*ao->bounds [i].length + ind [i] - 
			ao->bounds [i].lower_bound;

	return ves_icall_System_Array_GetValueImpl (this, pos);
}

static void
ves_icall_System_Array_SetValueImpl (MonoArray *this, MonoObject *value, guint32 pos)
{
	MonoClass *ac, *vc, *ec;
	gint32 esize, vsize;
	gpointer *ea, *va;

	guint64 u64 = 0;
	gint64 i64 = 0;
	gdouble r64 = 0;

	MONO_ARCH_SAVE_REGS;

	if (value)
		vc = value->vtable->klass;
	else
		vc = NULL;

	ac = this->obj.vtable->klass;
	ec = ac->element_class;

	esize = mono_array_element_size (ac);
	ea = (gpointer*)((char*)this->vector + (pos * esize));
	va = (gpointer*)((char*)value + sizeof (MonoObject));

	if (!value) {
		memset (ea, 0,  esize);
		return;
	}

#define NO_WIDENING_CONVERSION G_STMT_START{\
	mono_raise_exception (mono_get_exception_argument ( \
		"value", "not a widening conversion")); \
}G_STMT_END

#define CHECK_WIDENING_CONVERSION(extra) G_STMT_START{\
	if (esize < vsize + (extra)) \
		mono_raise_exception (mono_get_exception_argument ( \
			"value", "not a widening conversion")); \
}G_STMT_END

#define INVALID_CAST G_STMT_START{\
	mono_raise_exception (mono_get_exception_invalid_cast ()); \
}G_STMT_END

	/* Check element (destination) type. */
	switch (ec->byval_arg.type) {
	case MONO_TYPE_STRING:
		switch (vc->byval_arg.type) {
		case MONO_TYPE_STRING:
			break;
		default:
			INVALID_CAST;
		}
		break;
	case MONO_TYPE_BOOLEAN:
		switch (vc->byval_arg.type) {
		case MONO_TYPE_BOOLEAN:
			break;
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
		case MONO_TYPE_U8:
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
		case MONO_TYPE_I4:
		case MONO_TYPE_I8:
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			NO_WIDENING_CONVERSION;
		default:
			INVALID_CAST;
		}
		break;
	}

	if (!ec->valuetype) {
		if (!mono_object_isinst (value, ec))
			INVALID_CAST;
		*ea = (gpointer)value;
		return;
	}

	if (mono_object_isinst (value, ec)) {
		memcpy (ea, (char *)value + sizeof (MonoObject), esize);
		return;
	}

	if (!vc->valuetype)
		INVALID_CAST;

	vsize = mono_class_instance_size (vc) - sizeof (MonoObject);

#if 0
	g_message (G_STRLOC ": %d (%d) <= %d (%d)",
		   ec->byval_arg.type, esize,
		   vc->byval_arg.type, vsize);
#endif

#define ASSIGN_UNSIGNED(etype) G_STMT_START{\
	switch (vc->byval_arg.type) { \
	case MONO_TYPE_U1: \
	case MONO_TYPE_U2: \
	case MONO_TYPE_U4: \
	case MONO_TYPE_U8: \
	case MONO_TYPE_CHAR: \
		CHECK_WIDENING_CONVERSION(0); \
		*(etype *) ea = (etype) u64; \
		return; \
	/* You can't assign a signed value to an unsigned array. */ \
	case MONO_TYPE_I1: \
	case MONO_TYPE_I2: \
	case MONO_TYPE_I4: \
	case MONO_TYPE_I8: \
	/* You can't assign a floating point number to an integer array. */ \
	case MONO_TYPE_R4: \
	case MONO_TYPE_R8: \
		NO_WIDENING_CONVERSION; \
	} \
}G_STMT_END

#define ASSIGN_SIGNED(etype) G_STMT_START{\
	switch (vc->byval_arg.type) { \
	case MONO_TYPE_I1: \
	case MONO_TYPE_I2: \
	case MONO_TYPE_I4: \
	case MONO_TYPE_I8: \
		CHECK_WIDENING_CONVERSION(0); \
		*(etype *) ea = (etype) i64; \
		return; \
	/* You can assign an unsigned value to a signed array if the array's */ \
	/* element size is larger than the value size. */ \
	case MONO_TYPE_U1: \
	case MONO_TYPE_U2: \
	case MONO_TYPE_U4: \
	case MONO_TYPE_U8: \
	case MONO_TYPE_CHAR: \
		CHECK_WIDENING_CONVERSION(1); \
		*(etype *) ea = (etype) u64; \
		return; \
	/* You can't assign a floating point number to an integer array. */ \
	case MONO_TYPE_R4: \
	case MONO_TYPE_R8: \
		NO_WIDENING_CONVERSION; \
	} \
}G_STMT_END

#define ASSIGN_REAL(etype) G_STMT_START{\
	switch (vc->byval_arg.type) { \
	case MONO_TYPE_R4: \
	case MONO_TYPE_R8: \
		CHECK_WIDENING_CONVERSION(0); \
		*(etype *) ea = (etype) r64; \
		return; \
	/* All integer values fit into a floating point array, so we don't */ \
	/* need to CHECK_WIDENING_CONVERSION here. */ \
	case MONO_TYPE_I1: \
	case MONO_TYPE_I2: \
	case MONO_TYPE_I4: \
	case MONO_TYPE_I8: \
		*(etype *) ea = (etype) i64; \
		return; \
	case MONO_TYPE_U1: \
	case MONO_TYPE_U2: \
	case MONO_TYPE_U4: \
	case MONO_TYPE_U8: \
	case MONO_TYPE_CHAR: \
		*(etype *) ea = (etype) u64; \
		return; \
	} \
}G_STMT_END

	switch (vc->byval_arg.type) {
	case MONO_TYPE_U1:
		u64 = *(guint8 *) va;
		break;
	case MONO_TYPE_U2:
		u64 = *(guint16 *) va;
		break;
	case MONO_TYPE_U4:
		u64 = *(guint32 *) va;
		break;
	case MONO_TYPE_U8:
		u64 = *(guint64 *) va;
		break;
	case MONO_TYPE_I1:
		i64 = *(gint8 *) va;
		break;
	case MONO_TYPE_I2:
		i64 = *(gint16 *) va;
		break;
	case MONO_TYPE_I4:
		i64 = *(gint32 *) va;
		break;
	case MONO_TYPE_I8:
		i64 = *(gint64 *) va;
		break;
	case MONO_TYPE_R4:
		r64 = *(gfloat *) va;
		break;
	case MONO_TYPE_R8:
		r64 = *(gdouble *) va;
		break;
	case MONO_TYPE_CHAR:
		u64 = *(guint16 *) va;
		break;
	case MONO_TYPE_BOOLEAN:
		/* Boolean is only compatible with itself. */
		switch (ec->byval_arg.type) {
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
		case MONO_TYPE_U8:
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
		case MONO_TYPE_I4:
		case MONO_TYPE_I8:
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			NO_WIDENING_CONVERSION;
		default:
			INVALID_CAST;
		}
		break;
	}

	/* If we can't do a direct copy, let's try a widening conversion. */
	switch (ec->byval_arg.type) {
	case MONO_TYPE_CHAR:
		ASSIGN_UNSIGNED (guint16);
	case MONO_TYPE_U1:
		ASSIGN_UNSIGNED (guint8);
	case MONO_TYPE_U2:
		ASSIGN_UNSIGNED (guint16);
	case MONO_TYPE_U4:
		ASSIGN_UNSIGNED (guint32);
	case MONO_TYPE_U8:
		ASSIGN_UNSIGNED (guint64);
	case MONO_TYPE_I1:
		ASSIGN_SIGNED (gint8);
	case MONO_TYPE_I2:
		ASSIGN_SIGNED (gint16);
	case MONO_TYPE_I4:
		ASSIGN_SIGNED (gint32);
	case MONO_TYPE_I8:
		ASSIGN_SIGNED (gint64);
	case MONO_TYPE_R4:
		ASSIGN_REAL (gfloat);
	case MONO_TYPE_R8:
		ASSIGN_REAL (gdouble);
	}

	INVALID_CAST;
	/* Not reached, INVALID_CAST does not return. Just to avoid a compiler warning ... */
	return;

#undef INVALID_CAST
#undef NO_WIDENING_CONVERSION
#undef CHECK_WIDENING_CONVERSION
#undef ASSIGN_UNSIGNED
#undef ASSIGN_SIGNED
#undef ASSIGN_REAL
}

static void 
ves_icall_System_Array_SetValue (MonoArray *this, MonoObject *value,
				 MonoArray *idxs)
{
	MonoClass *ac, *ic;
	gint32 i, pos, *ind;

	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (idxs);

	ic = idxs->obj.vtable->klass;
	ac = this->obj.vtable->klass;

	g_assert (ic->rank == 1);
	if (idxs->bounds != NULL || idxs->max_length != ac->rank)
		mono_raise_exception (mono_get_exception_argument (NULL, NULL));

	ind = (guint32 *)idxs->vector;

	if (this->bounds == NULL) {
		if (*ind < 0 || *ind >= this->max_length)
			mono_raise_exception (mono_get_exception_index_out_of_range ());

		ves_icall_System_Array_SetValueImpl (this, value, *ind);
		return;
	}
	
	for (i = 0; i < ac->rank; i++)
		if ((ind [i] < this->bounds [i].lower_bound) ||
		    (ind [i] >= this->bounds [i].length + this->bounds [i].lower_bound))
			mono_raise_exception (mono_get_exception_index_out_of_range ());

	pos = ind [0] - this->bounds [0].lower_bound;
	for (i = 1; i < ac->rank; i++)
		pos = pos * this->bounds [i].length + ind [i] - 
			this->bounds [i].lower_bound;

	ves_icall_System_Array_SetValueImpl (this, value, pos);
}

static MonoArray *
ves_icall_System_Array_CreateInstanceImpl (MonoReflectionType *type, MonoArray *lengths, MonoArray *bounds)
{
	MonoClass *aklass;
	MonoArray *array;
	gint32 *sizes, i;
	gboolean bounded = FALSE;

	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (type);
	MONO_CHECK_ARG_NULL (lengths);

	MONO_CHECK_ARG (lengths, mono_array_length (lengths) > 0);
	if (bounds)
		MONO_CHECK_ARG (bounds, mono_array_length (lengths) == mono_array_length (bounds));

	for (i = 0; i < mono_array_length (lengths); i++)
		if (mono_array_get (lengths, gint32, i) < 0)
			mono_raise_exception (mono_get_exception_argument_out_of_range (NULL));

	if (bounds && (mono_array_length (bounds) == 1) && (mono_array_get (bounds, gint32, 0) != 0))
		/* vectors are not the same as one dimensional arrays with no-zero bounds */
		bounded = TRUE;
	else
		bounded = FALSE;

	aklass = mono_bounded_array_class_get (mono_class_from_mono_type (type->type), mono_array_length (lengths), bounded);

	sizes = alloca (aklass->rank * sizeof(guint32) * 2);
	for (i = 0; i < aklass->rank; ++i) {
		sizes [i] = mono_array_get (lengths, gint32, i);
		if (bounds)
			sizes [i + aklass->rank] = mono_array_get (bounds, gint32, i);
		else
			sizes [i + aklass->rank] = 0;
	}

	array = mono_array_new_full (mono_object_domain (type), aklass, sizes, sizes + aklass->rank);

	return array;
}

static gint32 
ves_icall_System_Array_GetRank (MonoObject *this)
{
	MONO_ARCH_SAVE_REGS;

	return this->vtable->klass->rank;
}

static gint32
ves_icall_System_Array_GetLength (MonoArray *this, gint32 dimension)
{
	gint32 rank = ((MonoObject *)this)->vtable->klass->rank;

	MONO_ARCH_SAVE_REGS;

	if ((dimension < 0) || (dimension >= rank))
		mono_raise_exception (mono_get_exception_index_out_of_range ());
	
	if (this->bounds == NULL)
		return this->max_length;
	
	return this->bounds [dimension].length;
}

static gint32
ves_icall_System_Array_GetLowerBound (MonoArray *this, gint32 dimension)
{
	gint32 rank = ((MonoObject *)this)->vtable->klass->rank;

	MONO_ARCH_SAVE_REGS;

	if ((dimension < 0) || (dimension >= rank))
		mono_raise_exception (mono_get_exception_index_out_of_range ());
	
	if (this->bounds == NULL)
		return 0;
	
	return this->bounds [dimension].lower_bound;
}

static gboolean
ves_icall_System_Array_FastCopy (MonoArray *source, int source_idx, MonoArray* dest, int dest_idx, int length)
{
	int element_size;
	void * dest_addr;
	void * source_addr;
	MonoClass *src_class;
	MonoClass *dest_class;
	int i;

	MONO_ARCH_SAVE_REGS;

	if (source->obj.vtable->klass->rank != dest->obj.vtable->klass->rank)
		return FALSE;

	if (source->bounds || dest->bounds)
		return FALSE;

	if ((dest_idx + length > mono_array_length (dest)) ||
		(source_idx + length > mono_array_length (source)))
		return FALSE;

	element_size = mono_array_element_size (source->obj.vtable->klass);
	dest_addr = mono_array_addr_with_size (dest, element_size, dest_idx);
	source_addr = mono_array_addr_with_size (source, element_size, source_idx);

	src_class = source->obj.vtable->klass->element_class;
	dest_class = dest->obj.vtable->klass->element_class;

	/*
	 * Handle common cases.
	 */

	/* Case1: object[] -> valuetype[] (ArrayList::ToArray) */
	if (src_class == mono_defaults.object_class && dest_class->valuetype) {
		for (i = source_idx; i < source_idx + length; ++i) {
			MonoObject *elem = mono_array_get (source, MonoObject*, i);
			if (elem && !mono_object_isinst (elem, dest_class))
				return FALSE;
		}

		element_size = mono_array_element_size (dest->obj.vtable->klass);
		for (i = 0; i < length; ++i) {
			MonoObject *elem = mono_array_get (source, MonoObject*, source_idx + i);
			void *addr = mono_array_addr_with_size (dest, element_size, dest_idx + i);
			if (!elem)
				memset (addr, 0, element_size);
			else
				memcpy (addr, (char *)elem + sizeof (MonoObject), element_size);
		}
		return TRUE;
	}

	if (src_class != dest_class) {
		if (dest_class->valuetype || dest_class->enumtype || src_class->valuetype || src_class->enumtype)
			return FALSE;

		if (mono_class_is_subclass_of (src_class, dest_class, FALSE))
			;
		/* Case2: object[] -> reftype[] (ArrayList::ToArray) */
		else if (mono_class_is_subclass_of (dest_class, src_class, FALSE))
			for (i = source_idx; i < source_idx + length; ++i) {
				MonoObject *elem = mono_array_get (source, MonoObject*, i);
				if (elem && !mono_object_isinst (elem, dest_class))
					return FALSE;
			}
		else
			return FALSE;
	}

	memmove (dest_addr, source_addr, element_size * length);

	return TRUE;
}

static void
ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_InitializeArray (MonoArray *array, MonoClassField *field_handle)
{
	MonoClass *klass = array->obj.vtable->klass;
	guint32 size = mono_array_element_size (klass);
	int i;

	MONO_ARCH_SAVE_REGS;

	if (array->bounds == NULL)
		size *= array->max_length;
	else
		for (i = 0; i < klass->rank; ++i) 
			size *= array->bounds [i].length;

	memcpy (mono_array_addr (array, char, 0), field_handle->data, size);

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#define SWAP(n) {\
	gint i; \
	guint ## n tmp; \
	guint ## n *data = (guint ## n *) mono_array_addr (array, char, 0); \
\
	for (i = 0; i < size; i += n/8, data++) { \
		tmp = read ## n (data); \
		*data = tmp; \
	} \
}

	/* printf ("Initialize array with elements of %s type\n", klass->element_class->name); */

	switch (klass->element_class->byval_arg.type) {
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		SWAP (16);
		break;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		SWAP (32);
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		SWAP (64);
		break;
	}
		 
#endif
}

static gint
ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_GetOffsetToStringData (void)
{
	MONO_ARCH_SAVE_REGS;

	return offsetof (MonoString, chars);
}

static MonoObject *
ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_GetObjectValue (MonoObject *obj)
{
	MONO_ARCH_SAVE_REGS;

	if ((obj == NULL) || (! (obj->vtable->klass->valuetype)))
		return obj;
	else
		return mono_object_clone (obj);
}

static void
ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_RunClassConstructor (MonoType *handle)
{
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (handle);

	klass = mono_class_from_mono_type (handle);
	MONO_CHECK_ARG (handle, klass);

	/* This will call the type constructor */
	if (! (klass->flags & TYPE_ATTRIBUTE_INTERFACE))
		mono_runtime_class_init (mono_class_vtable (mono_domain_get (), klass));
}

static MonoObject *
ves_icall_System_Object_MemberwiseClone (MonoObject *this)
{
	MONO_ARCH_SAVE_REGS;

	return mono_object_clone (this);
}

#if HAVE_BOEHM_GC
#define MONO_OBJECT_ALIGNMENT_SHIFT	3
#else
#define MONO_OBJECT_ALIGNMENT_SHIFT	2
#endif

/*
 * Return hashcode based on object address. This function will need to be
 * smarter in the presence of a moving garbage collector, which will cache
 * the address hash before relocating the object.
 *
 * Wang's address-based hash function:
 *   http://www.concentric.net/~Ttwang/tech/addrhash.htm
 */
static gint32
ves_icall_System_Object_GetHashCode (MonoObject *this)
{
	register guint32 key;

	MONO_ARCH_SAVE_REGS;

	key = (GPOINTER_TO_UINT (this) >> MONO_OBJECT_ALIGNMENT_SHIFT) * 2654435761u;

	return key & 0x7fffffff;
}

static gint32
ves_icall_System_ValueType_InternalGetHashCode (MonoObject *this, MonoArray **fields)
{
	int i;
	MonoClass *klass;
	MonoObject **values = NULL;
	MonoObject *o;
	int count = 0;
	gint32 result = 0;

	MONO_ARCH_SAVE_REGS;

	klass = this->vtable->klass;

	if (klass->field.count == 0)
		return ves_icall_System_Object_GetHashCode (this);

	/*
	 * Compute the starting value of the hashcode for fields of primitive
	 * types, and return the remaining fields in an array to the managed side.
	 * This way, we can avoid costly reflection operations in managed code.
	 */
	for (i = 0; i < klass->field.count; ++i) {
		MonoClassField *field = &klass->fields [i];
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;
		if (mono_field_is_deleted (field))
			continue;
		/* FIXME: Add more types */
		switch (field->type->type) {
		case MONO_TYPE_I4:
			result ^= *(gint32*)((guint8*)this + field->offset);
			break;
		case MONO_TYPE_STRING: {
			MonoString *s;
			s = *(MonoString**)((guint8*)this + field->offset);
			if (s != NULL)
				result ^= ves_icall_System_String_GetHashCode (s);
			break;
		}
		default:
			if (!values)
				values = alloca (klass->field.count * sizeof (MonoObject*));
			o = mono_field_get_value_object (mono_object_domain (this), field, this);
			values [count++] = o;
		}
	}

	if (values) {
		*fields = mono_array_new (mono_domain_get (), mono_defaults.object_class, count);
		memcpy (mono_array_addr (*fields, MonoObject*, 0), values, count * sizeof (MonoObject*));
	}
	else
		*fields = NULL;
	return result;
}

static MonoBoolean
ves_icall_System_ValueType_Equals (MonoObject *this, MonoObject *that, MonoArray **fields)
{
	int i;
	MonoClass *klass;
	MonoObject **values = NULL;
	MonoObject *o;
	int count = 0;

	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (that);

	if (this->vtable != that->vtable)
		return FALSE;

	klass = this->vtable->klass;

	/*
	 * Do the comparison for fields of primitive type and return a result if
	 * possible. Otherwise, return the remaining fields in an array to the 
	 * managed side. This way, we can avoid costly reflection operations in 
	 * managed code.
	 */
	*fields = NULL;
	for (i = 0; i < klass->field.count; ++i) {
		MonoClassField *field = &klass->fields [i];
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;
		if (mono_field_is_deleted (field))
			continue;
		/* FIXME: Add more types */
		switch (field->type->type) {
		case MONO_TYPE_I4:
			if (*(gint32*)((guint8*)this + field->offset) != *(gint32*)((guint8*)that + field->offset))
				return FALSE;
			break;
		case MONO_TYPE_STRING: {
			MonoString *s1, *s2;
			guint32 s1len, s2len;
			s1 = *(MonoString**)((guint8*)this + field->offset);
			s2 = *(MonoString**)((guint8*)that + field->offset);
			if (s1 == s2)
				break;
			if ((s1 == NULL) || (s2 == NULL))
				return FALSE;
			s1len = mono_string_length (s1);
			s2len = mono_string_length (s2);
			if (s1len != s2len)
				return FALSE;

			if (memcmp (mono_string_chars (s1), mono_string_chars (s2), s1len * sizeof (gunichar2)) != 0)
				return FALSE;
			break;
		}
		default:
			if (!values)
				values = alloca (klass->field.count * 2 * sizeof (MonoObject*));
			o = mono_field_get_value_object (mono_object_domain (this), field, this);
			values [count++] = o;
			o = mono_field_get_value_object (mono_object_domain (this), field, that);
			values [count++] = o;
		}
	}

	if (values) {
		*fields = mono_array_new (mono_domain_get (), mono_defaults.object_class, count);
		memcpy (mono_array_addr (*fields, MonoObject*, 0), values, count * sizeof (MonoObject*));

		return FALSE;
	}
	else
		return TRUE;
}

static MonoReflectionType *
ves_icall_System_Object_GetType (MonoObject *obj)
{
	MONO_ARCH_SAVE_REGS;

	if (obj->vtable->klass != mono_defaults.transparent_proxy_class)
		return mono_type_get_object (mono_object_domain (obj), &obj->vtable->klass->byval_arg);
	else
		return mono_type_get_object (mono_object_domain (obj), &((MonoTransparentProxy*)obj)->remote_class->proxy_class->byval_arg);
}

static void
mono_type_type_from_obj (MonoReflectionType *mtype, MonoObject *obj)
{
	MONO_ARCH_SAVE_REGS;

	mtype->type = &obj->vtable->klass->byval_arg;
	g_assert (mtype->type->type);
}

static gint32
ves_icall_ModuleBuilder_getToken (MonoReflectionModuleBuilder *mb, MonoObject *obj)
{
	MONO_ARCH_SAVE_REGS;

	return mono_image_create_token (mb->dynamic_image, obj);
}

static gint32
ves_icall_ModuleBuilder_getDataChunk (MonoReflectionModuleBuilder *mb, MonoArray *buf, gint32 offset)
{
	int count;
	MonoDynamicImage *image = mb->dynamic_image;
	char *p = mono_array_addr (buf, char, 0);

	MONO_ARCH_SAVE_REGS;

	mono_image_create_pefile (mb);

	if (offset >= image->pefile.index)
		return 0;
	count = mono_array_length (buf);
	count = MIN (count, image->pefile.index - offset);
	
	memcpy (p, image->pefile.data + offset, count);

	return count;
}

static void
ves_icall_ModuleBuilder_build_metadata (MonoReflectionModuleBuilder *mb)
{
	MONO_ARCH_SAVE_REGS;

	mono_image_build_metadata (mb);
}

static MonoReflectionType*
ves_icall_type_from_name (MonoString *name,
			  MonoBoolean throwOnError,
			  MonoBoolean ignoreCase)
{
	gchar *str;
	MonoType *type = NULL;
	MonoAssembly *assembly;
	MonoTypeNameParse info;

	MONO_ARCH_SAVE_REGS;

	str = mono_string_to_utf8 (name);
	if (!mono_reflection_parse_type (str, &info)) {
		g_free (str);
		g_list_free (info.modifiers);
		g_list_free (info.nested);
		if (throwOnError) /* uhm: this is a parse error, though... */
			mono_raise_exception (mono_get_exception_type_load (name));

		return NULL;
	}

	if (info.assembly.name) {
		assembly = mono_assembly_load (&info.assembly, NULL, NULL);
	} else {
		MonoReflectionAssembly *refass;

		refass = ves_icall_System_Reflection_Assembly_GetCallingAssembly  ();
		assembly = refass->assembly;
	}

	if (assembly)
		type = mono_reflection_get_type (assembly->image, &info, ignoreCase);
	
	if (!info.assembly.name && !type) /* try mscorlib */
		type = mono_reflection_get_type (NULL, &info, ignoreCase);

	g_free (str);
	g_list_free (info.modifiers);
	g_list_free (info.nested);
	if (!type) {
		if (throwOnError)
			mono_raise_exception (mono_get_exception_type_load (name));

		return NULL;
	}

	return mono_type_get_object (mono_domain_get (), type);
}

static MonoReflectionType*
ves_icall_type_from_handle (MonoType *handle)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *klass = mono_class_from_mono_type (handle);

	MONO_ARCH_SAVE_REGS;

	mono_class_init (klass);
	return mono_type_get_object (domain, handle);
}

static guint32
ves_icall_type_Equals (MonoReflectionType *type, MonoReflectionType *c)
{
	MONO_ARCH_SAVE_REGS;

	if (type->type && c->type)
		return mono_metadata_type_equal (type->type, c->type);
	g_print ("type equals\n");
	return 0;
}

/* System.TypeCode */
typedef enum {
	TYPECODE_EMPTY,
	TYPECODE_OBJECT,
	TYPECODE_DBNULL,
	TYPECODE_BOOLEAN,
	TYPECODE_CHAR,
	TYPECODE_SBYTE,
	TYPECODE_BYTE,
	TYPECODE_INT16,
	TYPECODE_UINT16,
	TYPECODE_INT32,
	TYPECODE_UINT32,
	TYPECODE_INT64,
	TYPECODE_UINT64,
	TYPECODE_SINGLE,
	TYPECODE_DOUBLE,
	TYPECODE_DECIMAL,
	TYPECODE_DATETIME,
	TYPECODE_STRING = 18
} TypeCode;

static guint32
ves_icall_type_GetTypeCode (MonoReflectionType *type)
{
	int t = type->type->type;

	MONO_ARCH_SAVE_REGS;

handle_enum:
	switch (t) {
	case MONO_TYPE_VOID:
		return TYPECODE_OBJECT;
	case MONO_TYPE_BOOLEAN:
		return TYPECODE_BOOLEAN;
	case MONO_TYPE_U1:
		return TYPECODE_BYTE;
	case MONO_TYPE_I1:
		return TYPECODE_SBYTE;
	case MONO_TYPE_U2:
		return TYPECODE_UINT16;
	case MONO_TYPE_I2:
		return TYPECODE_INT16;
	case MONO_TYPE_CHAR:
		return TYPECODE_CHAR;
	case MONO_TYPE_PTR:
	case MONO_TYPE_U:
	case MONO_TYPE_I:
		return TYPECODE_OBJECT;
	case MONO_TYPE_U4:
		return TYPECODE_UINT32;
	case MONO_TYPE_I4:
		return TYPECODE_INT32;
	case MONO_TYPE_U8:
		return TYPECODE_UINT64;
	case MONO_TYPE_I8:
		return TYPECODE_INT64;
	case MONO_TYPE_R4:
		return TYPECODE_SINGLE;
	case MONO_TYPE_R8:
		return TYPECODE_DOUBLE;
	case MONO_TYPE_VALUETYPE:
		if (type->type->data.klass->enumtype) {
			t = type->type->data.klass->enum_basetype->type;
			goto handle_enum;
		} else {
			MonoClass *k =  type->type->data.klass;
			if (strcmp (k->name_space, "System") == 0) {
				if (strcmp (k->name, "Decimal") == 0)
					return TYPECODE_DECIMAL;
				else if (strcmp (k->name, "DateTime") == 0)
					return TYPECODE_DATETIME;
			}
		}
		return TYPECODE_OBJECT;
	case MONO_TYPE_STRING:
		return TYPECODE_STRING;
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return TYPECODE_OBJECT;
	case MONO_TYPE_CLASS:
		{
			MonoClass *k =  type->type->data.klass;
			if (strcmp (k->name_space, "System") == 0) {
				if (strcmp (k->name, "DBNull") == 0)
					return TYPECODE_DBNULL;
			}
		}
		return TYPECODE_OBJECT;
	case MONO_TYPE_GENERICINST:
		return TYPECODE_OBJECT;
	default:
		g_error ("type 0x%02x not handled in GetTypeCode()", t);
	}
	return 0;
}

static guint32
ves_icall_type_is_subtype_of (MonoReflectionType *type, MonoReflectionType *c, MonoBoolean check_interfaces)
{
	MonoDomain *domain; 
	MonoClass *klass;
	MonoClass *klassc;

	MONO_ARCH_SAVE_REGS;

	g_assert (type != NULL);
	
	domain = ((MonoObject *)type)->vtable->domain;

	if (!c) /* FIXME: dont know what do do here */
		return 0;

	klass = mono_class_from_mono_type (type->type);
	klassc = mono_class_from_mono_type (c->type);

	return mono_class_is_subclass_of (klass, klassc, check_interfaces);
}

static guint32
ves_icall_type_is_assignable_from (MonoReflectionType *type, MonoReflectionType *c)
{
	MonoDomain *domain; 
	MonoClass *klass;
	MonoClass *klassc;

	MONO_ARCH_SAVE_REGS;

	g_assert (type != NULL);
	
	domain = ((MonoObject *)type)->vtable->domain;

	klass = mono_class_from_mono_type (type->type);
	klassc = mono_class_from_mono_type (c->type);

	return mono_class_is_assignable_from (klass, klassc);
}

static guint32
ves_icall_type_IsInstanceOfType (MonoReflectionType *type, MonoObject *obj)
{
	MonoClass *klass = mono_class_from_mono_type (type->type);
	return mono_object_isinst (obj, klass) != NULL;
}

static guint32
ves_icall_get_attributes (MonoReflectionType *type)
{
	MonoClass *klass = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return klass->flags;
}

static MonoReflectionField*
ves_icall_System_Reflection_FieldInfo_internal_from_handle (MonoClassField *handle)
{
	MONO_ARCH_SAVE_REGS;

	g_assert (handle);

	return mono_field_get_object (mono_domain_get (), handle->parent, handle);
}

static void
ves_icall_get_method_info (MonoMethod *method, MonoMethodInfo *info)
{
	MonoDomain *domain = mono_domain_get ();

	MONO_ARCH_SAVE_REGS;

	info->parent = mono_type_get_object (domain, &method->klass->byval_arg);
	info->ret = mono_type_get_object (domain, method->signature->ret);
	info->attrs = method->flags;
	info->implattrs = method->iflags;
	if (method->signature->call_convention == MONO_CALL_DEFAULT)
		info->callconv = 1;
	else
		if (method->signature->call_convention == MONO_CALL_VARARG)
			info->callconv = 2;
		else
			info->callconv = 0;
	info->callconv |= (method->signature->hasthis << 5) | (method->signature->explicit_this << 6); 
}

static MonoArray*
ves_icall_get_parameter_info (MonoMethod *method)
{
	MonoDomain *domain = mono_domain_get (); 

	MONO_ARCH_SAVE_REGS;

	return mono_param_get_objects (domain, method);
}

static MonoReflectionType*
ves_icall_MonoField_GetParentType (MonoReflectionField *field, MonoBoolean declaring)
{
	MonoClass *parent;
	MONO_ARCH_SAVE_REGS;

	parent = declaring? field->field->parent: field->klass;

	return mono_type_get_object (mono_object_domain (field), &parent->byval_arg);
}

static MonoObject *
ves_icall_MonoField_GetValueInternal (MonoReflectionField *field, MonoObject *obj)
{	
	MonoObject *o;
	MonoClassField *cf = field->field;
	MonoClass *klass;
	MonoVTable *vtable;
	MonoDomain *domain = mono_object_domain (field); 
	gchar *v;
	gboolean is_static = FALSE;
	gboolean is_ref = FALSE;

	MONO_ARCH_SAVE_REGS;

	mono_class_init (field->klass);

	switch (cf->type->type) {
	case MONO_TYPE_STRING:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
		is_ref = TRUE;
		break;
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U:
	case MONO_TYPE_I:
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_R4:
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
	case MONO_TYPE_R8:
	case MONO_TYPE_VALUETYPE:
		is_ref = cf->type->byref;
		break;
	default:
		g_error ("type 0x%x not handled in "
			 "ves_icall_Monofield_GetValue", cf->type->type);
		return NULL;
	}

	vtable = NULL;
	if (cf->type->attrs & FIELD_ATTRIBUTE_STATIC) {
		is_static = TRUE;
		vtable = mono_class_vtable (domain, field->klass);
		if (!vtable->initialized && !(cf->type->attrs & FIELD_ATTRIBUTE_LITERAL))
			mono_runtime_class_init (vtable);
	}
	
	if (is_ref) {
		if (is_static) {
			mono_field_static_get_value (vtable, cf, &o);
		} else {
			mono_field_get_value (obj, cf, &o);
		}
		return o;
	}

	/* boxed value type */
	klass = mono_class_from_mono_type (cf->type);
	o = mono_object_new (domain, klass);
	v = ((gchar *) o) + sizeof (MonoObject);
	if (is_static) {
		mono_field_static_get_value (vtable, cf, v);
	} else {
		mono_field_get_value (obj, cf, v);
	}

	return o;
}

static void
ves_icall_FieldInfo_SetValueInternal (MonoReflectionField *field, MonoObject *obj, MonoObject *value)
{
	MonoClassField *cf = field->field;
	gchar *v;

	MONO_ARCH_SAVE_REGS;

	v = (gchar *) value;
	if (!cf->type->byref) {
		switch (cf->type->type) {
		case MONO_TYPE_U1:
		case MONO_TYPE_I1:
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_U2:
		case MONO_TYPE_I2:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U:
		case MONO_TYPE_I:
		case MONO_TYPE_U4:
		case MONO_TYPE_I4:
		case MONO_TYPE_R4:
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
		case MONO_TYPE_R8:
		case MONO_TYPE_VALUETYPE:
			v += sizeof (MonoObject);
			break;
		case MONO_TYPE_STRING:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_SZARRAY:
			/* Do nothing */
			break;
		default:
			g_error ("type 0x%x not handled in "
				 "ves_icall_FieldInfo_SetValueInternal", cf->type->type);
			return;
		}
	}

	if (cf->type->attrs & FIELD_ATTRIBUTE_STATIC) {
		MonoVTable *vtable = mono_class_vtable (mono_object_domain (field), field->klass);
		if (!vtable->initialized)
			mono_runtime_class_init (vtable);
		mono_field_static_set_value (vtable, cf, v);
	} else {
		mono_field_set_value (obj, cf, v);
	}
}

/* From MonoProperty.cs */
typedef enum {
	PInfo_Attributes = 1,
	PInfo_GetMethod  = 1 << 1,
	PInfo_SetMethod  = 1 << 2,
	PInfo_ReflectedType = 1 << 3,
	PInfo_DeclaringType = 1 << 4,
	PInfo_Name = 1 << 5
} PInfo;

static void
ves_icall_get_property_info (MonoReflectionProperty *property, MonoPropertyInfo *info, PInfo req_info)
{
	MonoDomain *domain = mono_object_domain (property); 

	MONO_ARCH_SAVE_REGS;

	if ((req_info & PInfo_ReflectedType) != 0)
		info->parent = mono_type_get_object (domain, &property->klass->byval_arg);
	else if ((req_info & PInfo_DeclaringType) != 0)
		info->parent = mono_type_get_object (domain, &property->property->parent->byval_arg);

	if ((req_info & PInfo_Name) != 0)
		info->name = mono_string_new (domain, property->property->name);

	if ((req_info & PInfo_Attributes) != 0)
		info->attrs = property->property->attrs;

	if ((req_info & PInfo_GetMethod) != 0)
		info->get = property->property->get ?
			    mono_method_get_object (domain, property->property->get, NULL): NULL;
	
	if ((req_info & PInfo_SetMethod) != 0)
		info->set = property->property->set ?
			    mono_method_get_object (domain, property->property->set, NULL): NULL;
	/* 
	 * There may be other methods defined for properties, though, it seems they are not exposed 
	 * in the reflection API 
	 */
}

static void
ves_icall_get_event_info (MonoReflectionEvent *event, MonoEventInfo *info)
{
	MonoDomain *domain = mono_object_domain (event); 

	MONO_ARCH_SAVE_REGS;

	info->declaring_type = mono_type_get_object (domain, &event->klass->byval_arg);
	info->reflected_type = mono_type_get_object (domain, &event->event->parent->byval_arg);

	info->name = mono_string_new (domain, event->event->name);
	info->attrs = event->event->attrs;
	info->add_method = event->event->add ? mono_method_get_object (domain, event->event->add, NULL): NULL;
	info->remove_method = event->event->remove ? mono_method_get_object (domain, event->event->remove, NULL): NULL;
	info->raise_method = event->event->raise ? mono_method_get_object (domain, event->event->raise, NULL): NULL;
}

static MonoArray*
ves_icall_Type_GetInterfaces (MonoReflectionType* type)
{
	MonoDomain *domain = mono_object_domain (type); 
	MonoArray *intf;
	int ninterf, i;
	MonoClass *class = mono_class_from_mono_type (type->type);
	MonoClass *parent;
	MonoBitSet *slots = mono_bitset_new (class->max_interface_id + 1, 0);

	MONO_ARCH_SAVE_REGS;

	if (class->rank) {
		/* GetInterfaces() returns an empty array in MS.NET (this may be a bug) */
		mono_bitset_free (slots);
		return mono_array_new (domain, mono_defaults.monotype_class, 0);
	}

	ninterf = 0;
	for (parent = class; parent; parent = parent->parent) {
		for (i = 0; i < parent->interface_count; ++i) {
			if (mono_bitset_test (slots, parent->interfaces [i]->interface_id))
				continue;

			mono_bitset_set (slots, parent->interfaces [i]->interface_id);
			++ninterf;
		}
	}

	intf = mono_array_new (domain, mono_defaults.monotype_class, ninterf);
	ninterf = 0;
	for (parent = class; parent; parent = parent->parent) {
		for (i = 0; i < parent->interface_count; ++i) {
			if (!mono_bitset_test (slots, parent->interfaces [i]->interface_id))
				continue;

			mono_bitset_clear (slots, parent->interfaces [i]->interface_id);
			mono_array_set (intf, gpointer, ninterf,
					mono_type_get_object (domain, &parent->interfaces [i]->byval_arg));
			++ninterf;
		}
	}

	mono_bitset_free (slots);
	return intf;
}

static void
ves_icall_Type_GetInterfaceMapData (MonoReflectionType *type, MonoReflectionType *iface, MonoArray **targets, MonoArray **methods)
{
	MonoClass *class = mono_class_from_mono_type (type->type);
	MonoClass *iclass = mono_class_from_mono_type (iface->type);
	MonoReflectionMethod *member;
	int i, len, ioffset;
	MonoDomain *domain;

	MONO_ARCH_SAVE_REGS;

	/* type doesn't implement iface: the exception is thrown in managed code */
	if ((iclass->interface_id > class->max_interface_id) || !class->interface_offsets [iclass->interface_id])
			return;

	len = iclass->method.count;
	ioffset = class->interface_offsets [iclass->interface_id];
	domain = mono_object_domain (type);
	*targets = mono_array_new (domain, mono_defaults.method_info_class, len);
	*methods = mono_array_new (domain, mono_defaults.method_info_class, len);
	for (i = 0; i < len; ++i) {
		member = mono_method_get_object (domain, iclass->methods [i], iclass);
		mono_array_set (*methods, gpointer, i, member);
		member = mono_method_get_object (domain, class->vtable [i + ioffset], class);
		mono_array_set (*targets, gpointer, i, member);
	}
}

static MonoReflectionType*
ves_icall_MonoType_GetElementType (MonoReflectionType *type)
{
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return mono_type_get_object (mono_object_domain (type), &class->byval_arg);
	if (class->enumtype && class->enum_basetype) /* types that are modifierd typebuilkders may not have enum_basetype set */
		return mono_type_get_object (mono_object_domain (type), class->enum_basetype);
	else if (class->element_class)
		return mono_type_get_object (mono_object_domain (type), &class->element_class->byval_arg);
	else
		return NULL;
}

static MonoReflectionType*
ves_icall_get_type_parent (MonoReflectionType *type)
{
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return class->parent ? mono_type_get_object (mono_object_domain (type), &class->parent->byval_arg): NULL;
}

static MonoBoolean
ves_icall_type_ispointer (MonoReflectionType *type)
{
	MONO_ARCH_SAVE_REGS;

	return type->type->type == MONO_TYPE_PTR;
}

static MonoBoolean
ves_icall_type_isprimitive (MonoReflectionType *type)
{
	MONO_ARCH_SAVE_REGS;

	return (!type->type->byref && (type->type->type >= MONO_TYPE_BOOLEAN) && (type->type->type <= MONO_TYPE_R8));
}

static MonoBoolean
ves_icall_type_isbyref (MonoReflectionType *type)
{
	MONO_ARCH_SAVE_REGS;

	return type->type->byref;
}

static MonoReflectionModule*
ves_icall_MonoType_get_Module (MonoReflectionType *type)
{
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return mono_module_get_object (mono_object_domain (type), class->image);
}

static MonoReflectionAssembly*
ves_icall_MonoType_get_Assembly (MonoReflectionType *type)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return mono_assembly_get_object (domain, class->image->assembly);
}

static MonoReflectionType*
ves_icall_MonoType_get_DeclaringType (MonoReflectionType *type)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return class->nested_in ? mono_type_get_object (domain, &class->nested_in->byval_arg) : NULL;
}

static MonoReflectionType*
ves_icall_MonoType_get_UnderlyingSystemType (MonoReflectionType *type)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	if (class->enumtype && class->enum_basetype) /* types that are modified typebuilders may not have enum_basetype set */
		return mono_type_get_object (domain, class->enum_basetype);
	else if (class->element_class)
		return mono_type_get_object (domain, &class->element_class->byval_arg);
	else
		return NULL;
}

static MonoString*
ves_icall_MonoType_get_Name (MonoReflectionType *type)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return mono_string_new (domain, class->name);
}

static MonoString*
ves_icall_MonoType_get_Namespace (MonoReflectionType *type)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	while (class->nested_in)
		class = class->nested_in;

	return mono_string_new (domain, class->name_space);
}

static gint32
ves_icall_MonoType_GetArrayRank (MonoReflectionType *type)
{
	MonoClass *class = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	return class->rank;
}

static MonoArray*
ves_icall_MonoType_GetGenericArguments (MonoReflectionType *type)
{
	MonoArray *res;
	MonoClass *klass, *pklass;
	int i;
	MONO_ARCH_SAVE_REGS;

	klass = mono_class_from_mono_type (type->type);

	if (type->type->byref) {
		res = mono_array_new (mono_object_domain (type), mono_defaults.monotype_class, 0);
	} else if (klass->gen_params) {
		res = mono_array_new (mono_object_domain (type), mono_defaults.monotype_class, klass->num_gen_params);
		for (i = 0; i < klass->num_gen_params; ++i) {
			pklass = mono_class_from_generic_parameter (&klass->gen_params [i], klass->image, FALSE);
			mono_array_set (res, gpointer, i, mono_type_get_object (mono_object_domain (type), &pklass->byval_arg));
		}
	} else if (klass->generic_inst) {
		MonoGenericInst *inst = klass->generic_inst;
		res = mono_array_new (mono_object_domain (type), mono_defaults.monotype_class, inst->type_argc);
		for (i = 0; i < inst->type_argc; ++i) {
			mono_array_set (res, gpointer, i, mono_type_get_object (mono_object_domain (type), inst->type_argv [i]));
		}
	} else {
		res = mono_array_new (mono_object_domain (type), mono_defaults.monotype_class, 0);
	}
	return res;
}

static gboolean
ves_icall_Type_get_IsGenericTypeDefinition (MonoReflectionType *type)
{
	MonoClass *klass;
	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return FALSE;
	klass = mono_class_from_mono_type (type->type);

	return klass->gen_params != NULL;
}

static MonoReflectionType*
ves_icall_Type_GetGenericTypeDefinition_impl (MonoReflectionType *type)
{
	MonoClass *klass;
	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return NULL;
	klass = mono_class_from_mono_type (type->type);
	if (klass->gen_params) {
		return type; /* check this one */
	}
	if (klass->generic_inst) {
		MonoType *generic_type = klass->generic_inst->generic_type;
		MonoClass *generic_class = mono_class_from_mono_type (generic_type);

		if (generic_class->wastypebuilder && generic_class->reflection_info)
			return generic_class->reflection_info;
		else
			return mono_type_get_object (mono_object_domain (type), generic_type);
	}
	return NULL;
}

static MonoReflectionType*
ves_icall_Type_BindGenericParameters (MonoReflectionType *type, MonoArray *type_array)
{
	MonoType *geninst, **types;
	int i, count;

	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return NULL;

	count = mono_array_length (type_array);
	types = g_new0 (MonoType *, count);

	for (i = 0; i < count; i++) {
		MonoReflectionType *t = mono_array_get (type_array, gpointer, i);
		types [i] = t->type;
	}

	geninst = mono_reflection_bind_generic_parameters (type, count, types);

	return mono_type_get_object (mono_object_domain (type), geninst);
}

static gboolean
ves_icall_Type_get_IsGenericInstance (MonoReflectionType *type)
{
	MonoClass *klass;
	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return FALSE;
	klass = mono_class_from_mono_type (type->type);
	return klass->generic_inst != NULL;
}

static gint32
ves_icall_Type_GetGenericParameterPosition (MonoReflectionType *type)
{
	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return -1;
	if (type->type->type == MONO_TYPE_VAR || type->type->type == MONO_TYPE_MVAR)
		return type->type->data.generic_param->num;
	return -1;
}

static MonoBoolean
ves_icall_MonoType_get_HasGenericArguments (MonoReflectionType *type)
{
	MonoClass *klass;
	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return FALSE;
	klass = mono_class_from_mono_type (type->type);
	if (klass->gen_params || klass->generic_inst)
		return TRUE;
	return FALSE;
}

static MonoBoolean
ves_icall_MonoType_get_IsGenericParameter (MonoReflectionType *type)
{
	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return FALSE;
	if (type->type->type == MONO_TYPE_VAR || type->type->type == MONO_TYPE_MVAR)
		return TRUE;
	return FALSE;
}

static MonoBoolean
ves_icall_TypeBuilder_get_IsGenericParameter (MonoReflectionTypeBuilder *tb)
{
	MONO_ARCH_SAVE_REGS;

	if (tb->type.type->byref)
		return FALSE;
	if (tb->type.type->type == MONO_TYPE_VAR || tb->type.type->type == MONO_TYPE_MVAR)
		return TRUE;
	return FALSE;
}

static MonoReflectionType*
ves_icall_MonoGenericInst_GetParentType (MonoReflectionGenericInst *type)
{
	MonoGenericInst *ginst;
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	ginst = type->type.type->data.generic_inst;
	if (!ginst || !ginst->parent || (ginst->parent->type != MONO_TYPE_GENERICINST))
		return NULL;

	klass = mono_class_from_mono_type (ginst->parent);
	if (!klass->generic_inst && !klass->gen_params)
		return NULL;

	return mono_type_get_object (mono_object_domain (type), ginst->parent);
}

static MonoArray*
ves_icall_MonoGenericInst_GetInterfaces (MonoReflectionGenericInst *type)
{
	static MonoClass *System_Reflection_MonoGenericInst;
	MonoGenericInst *ginst;
	MonoDomain *domain;
	MonoClass *klass;
	MonoArray *res;
	int i;

	MONO_ARCH_SAVE_REGS;

	if (!System_Reflection_MonoGenericInst) {
		System_Reflection_MonoGenericInst = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "MonoGenericInst");
		g_assert (System_Reflection_MonoGenericInst);
	}

	domain = mono_object_domain (type);

	ginst = type->type.type->data.generic_inst;
	if (!ginst || !ginst->ifaces)
		return mono_array_new (domain, System_Reflection_MonoGenericInst, 0);

	klass = mono_class_from_mono_type (ginst->generic_type);

	res = mono_array_new (domain, System_Reflection_MonoGenericInst, ginst->count_ifaces);

	for (i = 0; i < ginst->count_ifaces; i++) {
		MonoReflectionType *iface = mono_type_get_object (domain, ginst->ifaces [i]);

		mono_array_set (res, gpointer, i, iface);
	}

	return res;
}

static MonoArray*
ves_icall_MonoGenericInst_GetMethods (MonoReflectionGenericInst *type,
				      MonoReflectionType *reflected_type)
{
	MonoGenericInst *ginst;
	MonoDynamicGenericInst *dginst;
	MonoDomain *domain;
	MonoClass *refclass;
	MonoArray *res;
	int i;

	MONO_ARCH_SAVE_REGS;

	ginst = type->type.type->data.generic_inst;
	g_assert ((dginst = ginst->dynamic_info) != NULL);

	refclass = mono_class_from_mono_type (reflected_type->type);

	domain = mono_object_domain (type);
	res = mono_array_new (domain, mono_defaults.method_info_class, dginst->count_methods);

	for (i = 0; i < dginst->count_methods; i++)
		mono_array_set (res, gpointer, i,
				mono_method_get_object (domain, dginst->methods [i], refclass));

	return res;
}

static MonoArray*
ves_icall_MonoGenericInst_GetConstructors (MonoReflectionGenericInst *type,
					   MonoReflectionType *reflected_type)
{
	static MonoClass *System_Reflection_ConstructorInfo;
	MonoGenericInst *ginst;
	MonoDynamicGenericInst *dginst;
	MonoDomain *domain;
	MonoClass *refclass;
	MonoArray *res;
	int i;

	MONO_ARCH_SAVE_REGS;

	if (!System_Reflection_ConstructorInfo)
		System_Reflection_ConstructorInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "ConstructorInfo");

	ginst = type->type.type->data.generic_inst;
	g_assert ((dginst = ginst->dynamic_info) != NULL);

	refclass = mono_class_from_mono_type (reflected_type->type);

	domain = mono_object_domain (type);
	res = mono_array_new (domain, System_Reflection_ConstructorInfo, dginst->count_ctors);

	for (i = 0; i < dginst->count_ctors; i++)
		mono_array_set (res, gpointer, i,
				mono_method_get_object (domain, dginst->ctors [i], refclass));

	return res;
}

static MonoArray*
ves_icall_MonoGenericInst_GetFields (MonoReflectionGenericInst *type,
				     MonoReflectionType *reflected_type)
{
	MonoGenericInst *ginst;
	MonoDynamicGenericInst *dginst;
	MonoDomain *domain;
	MonoClass *refclass;
	MonoArray *res;
	int i;

	MONO_ARCH_SAVE_REGS;

	ginst = type->type.type->data.generic_inst;
	g_assert ((dginst = ginst->dynamic_info) != NULL);

	refclass = mono_class_from_mono_type (reflected_type->type);

	domain = mono_object_domain (type);
	res = mono_array_new (domain, mono_defaults.field_info_class, dginst->count_fields);

	for (i = 0; i < dginst->count_fields; i++)
		mono_array_set (res, gpointer, i,
				mono_field_get_object (domain, refclass, &dginst->fields [i]));

	return res;
}

static MonoArray*
ves_icall_MonoGenericInst_GetProperties (MonoReflectionGenericInst *type,
					 MonoReflectionType *reflected_type)
{
	static MonoClass *System_Reflection_PropertyInfo;
	MonoGenericInst *ginst;
	MonoDynamicGenericInst *dginst;
	MonoDomain *domain;
	MonoClass *refclass;
	MonoArray *res;
	int i;

	MONO_ARCH_SAVE_REGS;

	if (!System_Reflection_PropertyInfo)
		System_Reflection_PropertyInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "PropertyInfo");

	ginst = type->type.type->data.generic_inst;
	g_assert ((dginst = ginst->dynamic_info) != NULL);

	refclass = mono_class_from_mono_type (reflected_type->type);

	domain = mono_object_domain (type);
	res = mono_array_new (domain, System_Reflection_PropertyInfo, dginst->count_properties);

	for (i = 0; i < dginst->count_properties; i++)
		mono_array_set (res, gpointer, i,
				mono_property_get_object (domain, refclass, &dginst->properties [i]));

	return res;
}

static MonoArray*
ves_icall_MonoGenericInst_GetEvents (MonoReflectionGenericInst *type,
				     MonoReflectionType *reflected_type)
{
	static MonoClass *System_Reflection_EventInfo;
	MonoGenericInst *ginst;
	MonoDynamicGenericInst *dginst;
	MonoDomain *domain;
	MonoClass *refclass;
	MonoArray *res;
	int i;

	MONO_ARCH_SAVE_REGS;

	if (!System_Reflection_EventInfo)
		System_Reflection_EventInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "EventInfo");

	ginst = type->type.type->data.generic_inst;
	g_assert ((dginst = ginst->dynamic_info) != NULL);

	refclass = mono_class_from_mono_type (reflected_type->type);

	domain = mono_object_domain (type);
	res = mono_array_new (domain, System_Reflection_EventInfo, dginst->count_events);

	for (i = 0; i < dginst->count_events; i++)
		mono_array_set (res, gpointer, i,
				mono_event_get_object (domain, refclass, &dginst->events [i]));

	return res;
}

static MonoReflectionMethod *
ves_icall_MonoType_get_DeclaringMethod (MonoReflectionType *type)
{
	MonoMethod *method;
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	if (type->type->byref)
		return FALSE;

	method = type->type->data.generic_param->method;
	if (!method)
		return NULL;

	klass = mono_class_from_mono_type (type->type);
	return mono_method_get_object (mono_object_domain (type), method, klass);
}

static MonoReflectionMethod *
ves_icall_MonoMethod_GetGenericMethodDefinition (MonoReflectionMethod *method)
{
	MonoMethodInflated *imethod;

	MONO_ARCH_SAVE_REGS;

	if (!method->method->signature->is_inflated) {
		if (method->method->signature->generic_param_count)
			return method;

		return NULL;
	}

	imethod = (MonoMethodInflated *) method->method;
	if (imethod->context->gmethod && imethod->context->gmethod->reflection_info)
		return imethod->context->gmethod->reflection_info;
	else
		return mono_method_get_object (
			mono_object_domain (method), imethod->declaring, NULL);
}

static gboolean
ves_icall_MonoMethod_get_HasGenericParameters (MonoReflectionMethod *method)
{
	MONO_ARCH_SAVE_REGS;

	if ((method->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
	    (method->method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		return FALSE;

	return method->method->signature->generic_param_count != 0;
}

static gboolean
ves_icall_MonoMethod_get_Mono_IsInflatedMethod (MonoReflectionMethod *method)
{
	MONO_ARCH_SAVE_REGS;

	if ((method->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
	    (method->method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		return FALSE;

	return method->method->signature->is_inflated;
}

static gboolean
ves_icall_MonoMethod_get_IsGenericMethodDefinition (MonoReflectionMethod *method)
{
	MONO_ARCH_SAVE_REGS;

	if ((method->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
	    (method->method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		return FALSE;

	return method->method->signature->generic_param_count != 0;
}

static MonoArray*
ves_icall_MonoMethod_GetGenericArguments (MonoReflectionMethod *method)
{
	MonoArray *res;
	MonoDomain *domain;
	MonoMethodNormal *mn;
	int count, i;
	MONO_ARCH_SAVE_REGS;

	domain = mono_object_domain (method);

	if ((method->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
	    (method->method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		return mono_array_new (domain, mono_defaults.monotype_class, 0);

	if (method->method->signature->is_inflated) {
		MonoMethodInflated *imethod = (MonoMethodInflated *) method->method;
		MonoGenericMethod *gmethod = imethod->context->gmethod;

		if (gmethod) {
			count = gmethod->mtype_argc;
			res = mono_array_new (domain, mono_defaults.monotype_class, count);

			for (i = 0; i < count; i++) {
				MonoType *t = gmethod->mtype_argv [i];
				mono_array_set (
					res, gpointer, i, mono_type_get_object (domain, t));
			}

			return res;
		}
	}

	mn = (MonoMethodNormal *) method->method;
	count = method->method->signature->generic_param_count;
	res = mono_array_new (domain, mono_defaults.monotype_class, count);

	for (i = 0; i < count; i++) {
		MonoGenericParam *param = &mn->header->gen_params [i];
		MonoClass *pklass = mono_class_from_generic_parameter (
			param, method->method->klass->image, TRUE);
		mono_array_set (res, gpointer, i,
				mono_type_get_object (domain, &pklass->byval_arg));
	}

	return res;
}

static MonoObject *
ves_icall_InternalInvoke (MonoReflectionMethod *method, MonoObject *this, MonoArray *params) 
{
	/* 
	 * Invoke from reflection is supposed to always be a virtual call (the API
	 * is stupid), mono_runtime_invoke_*() calls the provided method, allowing
	 * greater flexibility.
	 */
	MonoMethod *m = method->method;
	int pcount;

	MONO_ARCH_SAVE_REGS;

	if (this) {
		if (!mono_object_isinst (this, m->klass))
			mono_raise_exception (mono_exception_from_name (mono_defaults.corlib, "System.Reflection", "TargetException"));
		m = mono_object_get_virtual_method (this, m);
	} else if (!(m->flags & METHOD_ATTRIBUTE_STATIC) && strcmp (m->name, ".ctor") && !m->wrapper_type)
		mono_raise_exception (mono_exception_from_name (mono_defaults.corlib, "System.Reflection", "TargetException"));

	pcount = params? mono_array_length (params): 0;
	if (pcount != m->signature->param_count)
		mono_raise_exception (mono_exception_from_name (mono_defaults.corlib, "System.Reflection", "TargetParameterCountException"));

	if (m->klass->rank && !strcmp (m->name, ".ctor")) {
		int i;
		guint32 *lengths;
		guint32 *lower_bounds;
		pcount = mono_array_length (params);
		lengths = alloca (sizeof (guint32) * pcount);
		for (i = 0; i < pcount; ++i)
			lengths [i] = *(gint32*) ((char*)mono_array_get (params, gpointer, i) + sizeof (MonoObject));

		if (m->klass->rank == pcount) {
			/* Only lengths provided. */
			lower_bounds = NULL;
		} else {
			g_assert (pcount == (m->klass->rank * 2));
			/* lower bounds are first. */
			lower_bounds = lengths;
			lengths += m->klass->rank;
		}

		return (MonoObject*)mono_array_new_full (mono_object_domain (params), m->klass, lengths, lower_bounds);
	}
	return mono_runtime_invoke_array (m, this, params, NULL);
}

static MonoObject *
ves_icall_InternalExecute (MonoReflectionMethod *method, MonoObject *this, MonoArray *params, MonoArray **outArgs) 
{
	MonoDomain *domain = mono_object_domain (method); 
	MonoMethod *m = method->method;
	MonoMethodSignature *sig = m->signature;
	MonoArray *out_args;
	MonoObject *result;
	int i, j, outarg_count = 0;

	MONO_ARCH_SAVE_REGS;

	if (m->klass == mono_defaults.object_class) {

		if (!strcmp (m->name, "FieldGetter")) {
			MonoClass *k = this->vtable->klass;
			MonoString *name = mono_array_get (params, MonoString *, 1);
			char *str;

			str = mono_string_to_utf8 (name);
		
			for (i = 0; i < k->field.count; i++) {
				if (!strcmp (k->fields [i].name, str)) {
					MonoClass *field_klass =  mono_class_from_mono_type (k->fields [i].type);
					if (field_klass->valuetype)
						result = mono_value_box (domain, field_klass,
									 (char *)this + k->fields [i].offset);
					else 
						result = *((gpointer *)((char *)this + k->fields [i].offset));
				
					g_assert (result);
					out_args = mono_array_new (domain, mono_defaults.object_class, 1);
					*outArgs = out_args;
					mono_array_set (out_args, gpointer, 0, result);
					g_free (str);
					return NULL;
				}
			}

			g_free (str);
			g_assert_not_reached ();

		} else if (!strcmp (m->name, "FieldSetter")) {
			MonoClass *k = this->vtable->klass;
			MonoString *name = mono_array_get (params, MonoString *, 1);
			int size, align;
			char *str;

			str = mono_string_to_utf8 (name);
		
			for (i = 0; i < k->field.count; i++) {
				if (!strcmp (k->fields [i].name, str)) {
					MonoClass *field_klass =  mono_class_from_mono_type (k->fields [i].type);
					MonoObject *val = mono_array_get (params, gpointer, 2);

					if (field_klass->valuetype) {
						size = mono_type_size (k->fields [i].type, &align);
						memcpy ((char *)this + k->fields [i].offset, 
							((char *)val) + sizeof (MonoObject), size);
					} else 
						*(MonoObject**)((char *)this + k->fields [i].offset) = val;
				
					out_args = mono_array_new (domain, mono_defaults.object_class, 0);
					*outArgs = out_args;

					g_free (str);
					return NULL;
				}
			}

			g_free (str);
			g_assert_not_reached ();

		}
	}

	for (i = 0; i < mono_array_length (params); i++) {
		if (sig->params [i]->byref) 
			outarg_count++;
	}

	out_args = mono_array_new (domain, mono_defaults.object_class, outarg_count);
	
	/* fixme: handle constructors? */
	if (!strcmp (method->method->name, ".ctor"))
		g_assert_not_reached ();

	result = mono_runtime_invoke_array (method->method, this, params, NULL);

	for (i = 0, j = 0; i < mono_array_length (params); i++) {
		if (sig->params [i]->byref) {
			gpointer arg;
			arg = mono_array_get (params, gpointer, i);
			mono_array_set (out_args, gpointer, j, arg);
			j++;
		}
	}

	*outArgs = out_args;

	return result;
}

static MonoObject *
ves_icall_System_Enum_ToObject (MonoReflectionType *type, MonoObject *obj)
{
	MonoDomain *domain; 
	MonoClass *enumc, *objc;
	gint32 s1, s2;
	MonoObject *res;
	
	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (type);
	MONO_CHECK_ARG_NULL (obj);

	domain = mono_object_domain (type); 
	enumc = mono_class_from_mono_type (type->type);
	objc = obj->vtable->klass;

	MONO_CHECK_ARG (obj, enumc->enumtype == TRUE);
	MONO_CHECK_ARG (obj, (objc->enumtype) || (objc->byval_arg.type >= MONO_TYPE_I1 &&
						  objc->byval_arg.type <= MONO_TYPE_U8));
	
	s1 = mono_class_value_size (enumc, NULL);
	s2 = mono_class_value_size (objc, NULL);

	res = mono_object_new (domain, enumc);

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	memcpy ((char *)res + sizeof (MonoObject), (char *)obj + sizeof (MonoObject), MIN (s1, s2));
#else
	memcpy ((char *)res + sizeof (MonoObject) + (s1 > s2 ? s1 - s2 : 0),
		(char *)obj + sizeof (MonoObject) + (s2 > s1 ? s2 - s1 : 0),
		MIN (s1, s2));
#endif
	return res;
}

static MonoObject *
ves_icall_System_Enum_get_value (MonoObject *this)
{
	MonoObject *res;
	MonoClass *enumc;
	gpointer dst;
	gpointer src;
	int size;

	MONO_ARCH_SAVE_REGS;

	if (!this)
		return NULL;

	g_assert (this->vtable->klass->enumtype);
	
	enumc = mono_class_from_mono_type (this->vtable->klass->enum_basetype);
	res = mono_object_new (mono_object_domain (this), enumc);
	dst = (char *)res + sizeof (MonoObject);
	src = (char *)this + sizeof (MonoObject);
	size = mono_class_value_size (enumc, NULL);

	memcpy (dst, src, size);

	return res;
}

static void
ves_icall_get_enum_info (MonoReflectionType *type, MonoEnumInfo *info)
{
	MonoDomain *domain = mono_object_domain (type); 
	MonoClass *enumc = mono_class_from_mono_type (type->type);
	guint i, j, nvalues, crow;
	MonoClassField *field;

	MONO_ARCH_SAVE_REGS;

	info->utype = mono_type_get_object (domain, enumc->enum_basetype);
	nvalues = enumc->field.count - 1;
	info->names = mono_array_new (domain, mono_defaults.string_class, nvalues);
	info->values = mono_array_new (domain, enumc, nvalues);
	
	crow = -1;
	for (i = 0, j = 0; i < enumc->field.count; ++i) {
		const char *p;
		int len;

		field = &enumc->fields [i];
		if (strcmp ("value__", field->name) == 0)
			continue;
		if (mono_field_is_deleted (field))
			continue;
		mono_array_set (info->names, gpointer, j, mono_string_new (domain, field->name));
		if (!field->def_value) {
			field->def_value = g_new0 (MonoConstant, 1);
			crow = mono_metadata_get_constant_index (enumc->image, MONO_TOKEN_FIELD_DEF | (i+enumc->field.first+1), crow + 1);
			field->def_value->type = mono_metadata_decode_row_col (&enumc->image->tables [MONO_TABLE_CONSTANT], crow-1, MONO_CONSTANT_TYPE);
			crow = mono_metadata_decode_row_col (&enumc->image->tables [MONO_TABLE_CONSTANT], crow-1, MONO_CONSTANT_VALUE);
			field->def_value->value = (gpointer)mono_metadata_blob_heap (enumc->image, crow);
		}

		p = field->def_value->value;
		len = mono_metadata_decode_blob_size (p, &p);
		switch (enumc->enum_basetype->type) {
		case MONO_TYPE_U1:
		case MONO_TYPE_I1:
			mono_array_set (info->values, gchar, j, *p);
			break;
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U2:
		case MONO_TYPE_I2:
			mono_array_set (info->values, gint16, j, read16 (p));
			break;
		case MONO_TYPE_U4:
		case MONO_TYPE_I4:
			mono_array_set (info->values, gint32, j, read32 (p));
			break;
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
			mono_array_set (info->values, gint64, j, read64 (p));
			break;
		default:
			g_error ("Implement type 0x%02x in get_enum_info", enumc->enum_basetype->type);
		}
		++j;
	}
}

enum {
	BFLAGS_IgnoreCase = 1,
	BFLAGS_DeclaredOnly = 2,
	BFLAGS_Instance = 4,
	BFLAGS_Static = 8,
	BFLAGS_Public = 0x10,
	BFLAGS_NonPublic = 0x20,
	BFLAGS_FlattenHierarchy = 0x40,
	BFLAGS_InvokeMethod = 0x100,
	BFLAGS_CreateInstance = 0x200,
	BFLAGS_GetField = 0x400,
	BFLAGS_SetField = 0x800,
	BFLAGS_GetProperty = 0x1000,
	BFLAGS_SetProperty = 0x2000,
	BFLAGS_ExactBinding = 0x10000,
	BFLAGS_SuppressChangeType = 0x20000,
	BFLAGS_OptionalParamBinding = 0x40000
};

static MonoReflectionField *
ves_icall_Type_GetField (MonoReflectionType *type, MonoString *name, guint32 bflags)
{
	MonoDomain *domain; 
	MonoClass *startklass, *klass;
	int i, match;
	MonoClassField *field;
	char *utf8_name;
	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

	MONO_ARCH_SAVE_REGS;

	if (!name)
		mono_raise_exception (mono_get_exception_argument_null ("name"));

handle_parent:	
	for (i = 0; i < klass->field.count; ++i) {
		match = 0;
		field = &klass->fields [i];
		if (mono_field_is_deleted (field))
			continue;
		if ((field->type->attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK) == FIELD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				if ((bflags & BFLAGS_FlattenHierarchy) || (klass == startklass))
					match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		
		utf8_name = mono_string_to_utf8 (name);

		if (strcmp (field->name, utf8_name)) {
			g_free (utf8_name);
			continue;
		}
		g_free (utf8_name);
		
		return mono_field_get_object (domain, startklass, field);
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;

	return NULL;
}

static MonoArray*
ves_icall_Type_GetFields_internal (MonoReflectionType *type, guint32 bflags, MonoReflectionType *reftype)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	MonoClass *startklass, *klass, *refklass;
	MonoArray *res;
	MonoObject *member;
	int i, len, match;
	MonoClassField *field;

	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);
	refklass = mono_class_from_mono_type (reftype->type);

handle_parent:	
	for (i = 0; i < klass->field.count; ++i) {
		match = 0;
		field = &klass->fields [i];
		if (mono_field_is_deleted (field))
			continue;
		if ((field->type->attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK) == FIELD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				if ((bflags & BFLAGS_FlattenHierarchy) || (klass == startklass))
					match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		member = (MonoObject*)mono_field_get_object (domain, refklass, field);
		l = g_slist_prepend (l, member);
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	len = g_slist_length (l);
	res = mono_array_new (domain, mono_defaults.field_info_class, len);
	i = 0;
	tmp = l = g_slist_reverse (l);
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoArray*
ves_icall_Type_GetMethodsByName (MonoReflectionType *type, MonoString *name, guint32 bflags, MonoBoolean ignore_case, MonoReflectionType *reftype)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	MonoClass *startklass, *klass, *refklass;
	MonoArray *res;
	MonoMethod *method;
	MonoObject *member;
	int i, len, match;
	GHashTable *method_slots = g_hash_table_new (NULL, NULL);
	gchar *mname = NULL;
	int (*compare_func) (const char *s1, const char *s2) = NULL;
		
	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);
	refklass = mono_class_from_mono_type (reftype->type);
	len = 0;
	if (name != NULL) {
		mname = mono_string_to_utf8 (name);
		compare_func = (ignore_case) ? g_strcasecmp : strcmp;
	}

handle_parent:
	for (i = 0; i < klass->method.count; ++i) {
		match = 0;
		method = klass->methods [i];
		if (strcmp (method->name, ".ctor") == 0 || strcmp (method->name, ".cctor") == 0)
			continue;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				if ((bflags & BFLAGS_FlattenHierarchy) || (klass == startklass))
					match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;

		if (name != NULL) {
			if (compare_func (mname, method->name))
				continue;
		}
		
		match = 0;
		if (method->slot != -1) {
			if (g_hash_table_lookup (method_slots, GUINT_TO_POINTER (method->slot)))
				continue;
			g_hash_table_insert (method_slots, GUINT_TO_POINTER (method->slot), method);
		}
		
		member = (MonoObject*)mono_method_get_object (domain, method, refklass);
		
		l = g_slist_prepend (l, member);
		len++;
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;

	g_free (mname);
	res = mono_array_new (domain, mono_defaults.method_info_class, len);
	i = 0;

	tmp = l = g_slist_reverse (l);

	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	g_hash_table_destroy (method_slots);
	return res;
}

static MonoArray*
ves_icall_Type_GetConstructors_internal (MonoReflectionType *type, guint32 bflags, MonoReflectionType *reftype)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_ConstructorInfo;
	MonoClass *startklass, *klass, *refklass;
	MonoArray *res;
	MonoMethod *method;
	MonoObject *member;
	int i, len, match;

	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);
	refklass = mono_class_from_mono_type (reftype->type);

	for (i = 0; i < klass->method.count; ++i) {
		match = 0;
		method = klass->methods [i];
		if (strcmp (method->name, ".ctor") && strcmp (method->name, ".cctor"))
			continue;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				if ((bflags & BFLAGS_FlattenHierarchy) || (klass == startklass))
					match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		member = (MonoObject*)mono_method_get_object (domain, method, refklass);
			
		l = g_slist_prepend (l, member);
	}
	len = g_slist_length (l);
	if (!System_Reflection_ConstructorInfo)
		System_Reflection_ConstructorInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "ConstructorInfo");
	res = mono_array_new (domain, System_Reflection_ConstructorInfo, len);
	i = 0;
	tmp = l = g_slist_reverse (l);
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoArray*
ves_icall_Type_GetPropertiesByName (MonoReflectionType *type, MonoString *name, guint32 bflags, MonoBoolean ignore_case, MonoReflectionType *reftype)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_PropertyInfo;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoMethod *method;
	MonoProperty *prop;
	int i, match;
	int len = 0;
	GHashTable *method_slots = g_hash_table_new (NULL, NULL);
	gchar *propname = NULL;
	int (*compare_func) (const char *s1, const char *s2) = NULL;

	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);
	if (name != NULL) {
		propname = mono_string_to_utf8 (name);
		compare_func = (ignore_case) ? g_strcasecmp : strcmp;
	}

handle_parent:
	for (i = 0; i < klass->property.count; ++i) {
		prop = &klass->properties [i];
		match = 0;
		method = prop->get;
		if (!method)
			method = prop->set;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				if ((bflags & BFLAGS_FlattenHierarchy) || (klass == startklass))
					match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		match = 0;

		if (name != NULL) {
			if (compare_func (propname, prop->name))
				continue;
		}
		
		if (prop->get && prop->get->slot != -1) {
			if (g_hash_table_lookup (method_slots, GUINT_TO_POINTER (prop->get->slot)))
				continue;
			g_hash_table_insert (method_slots, GUINT_TO_POINTER (prop->get->slot), prop);
		}
		if (prop->set && prop->set->slot != -1) {
			if (g_hash_table_lookup (method_slots, GUINT_TO_POINTER (prop->set->slot)))
				continue;
			g_hash_table_insert (method_slots, GUINT_TO_POINTER (prop->set->slot), prop);
		}

		l = g_slist_prepend (l, mono_property_get_object (domain, startklass, prop));
		len++;
	}
	if ((!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent)))
		goto handle_parent;

	g_free (propname);
	if (!System_Reflection_PropertyInfo)
		System_Reflection_PropertyInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "PropertyInfo");
	res = mono_array_new (domain, System_Reflection_PropertyInfo, len);
	i = 0;

	tmp = l = g_slist_reverse (l);

	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	g_hash_table_destroy (method_slots);
	return res;
}

static MonoReflectionEvent *
ves_icall_MonoType_GetEvent (MonoReflectionType *type, MonoString *name, guint32 bflags)
{
	MonoDomain *domain;
	MonoClass *klass, *startklass;
	gint i;
	MonoEvent *event;
	MonoMethod *method;
	gchar *event_name;

	MONO_ARCH_SAVE_REGS;

	event_name = mono_string_to_utf8 (name);
	klass = startklass = mono_class_from_mono_type (type->type);
	domain = mono_object_domain (type);

handle_parent:	
	for (i = 0; i < klass->event.count; i++) {
		event = &klass->events [i];
		if (strcmp (event->name, event_name))
			continue;

		method = event->add;
		if (!method)
			method = event->remove;

		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (!(bflags & BFLAGS_Public))
				continue;
		} else {
			if (!(bflags & BFLAGS_NonPublic))
				continue;
		}

		g_free (event_name);
		return mono_event_get_object (domain, startklass, event);
	}

	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;

	g_free (event_name);
	return NULL;
}

static MonoArray*
ves_icall_Type_GetEvents_internal (MonoReflectionType *type, guint32 bflags, MonoReflectionType *reftype)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_EventInfo;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoMethod *method;
	MonoEvent *event;
	int i, len, match;

	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

handle_parent:	
	for (i = 0; i < klass->event.count; ++i) {
		event = &klass->events [i];
		match = 0;
		method = event->add;
		if (!method)
			method = event->remove;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				if ((bflags & BFLAGS_FlattenHierarchy) || (klass == startklass))
					match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		match = 0;
		l = g_slist_prepend (l, mono_event_get_object (domain, klass, event));
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	len = g_slist_length (l);
	if (!System_Reflection_EventInfo)
		System_Reflection_EventInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "EventInfo");
	res = mono_array_new (domain, System_Reflection_EventInfo, len);
	i = 0;

	tmp = l = g_slist_reverse (l);

	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoReflectionType *
ves_icall_Type_GetNestedType (MonoReflectionType *type, MonoString *name, guint32 bflags)
{
	MonoDomain *domain; 
	MonoClass *startklass, *klass;
	MonoClass *nested;
	GList *tmpn;
	char *str;
	
	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);
	str = mono_string_to_utf8 (name);

 handle_parent:
	for (tmpn = klass->nested_classes; tmpn; tmpn = tmpn->next) {
		int match = 0;
		nested = tmpn->data;
		if ((nested->flags & TYPE_ATTRIBUTE_VISIBILITY_MASK) == TYPE_ATTRIBUTE_NESTED_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		if (strcmp (nested->name, str) == 0){
			g_free (str);
			return mono_type_get_object (domain, &nested->byval_arg);
		}
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	g_free (str);
	return NULL;
}

static MonoArray*
ves_icall_Type_GetNestedTypes (MonoReflectionType *type, guint32 bflags)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	GList *tmpn;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoObject *member;
	int i, len, match;
	MonoClass *nested;

	MONO_ARCH_SAVE_REGS;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

	for (tmpn = klass->nested_classes; tmpn; tmpn = tmpn->next) {
		match = 0;
		nested = tmpn->data;
		if ((nested->flags & TYPE_ATTRIBUTE_VISIBILITY_MASK) == TYPE_ATTRIBUTE_NESTED_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		member = (MonoObject*)mono_type_get_object (domain, &nested->byval_arg);
		l = g_slist_prepend (l, member);
	}
	len = g_slist_length (l);
	res = mono_array_new (domain, mono_defaults.monotype_class, len);
	i = 0;
	tmp = l = g_slist_reverse (l);
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoReflectionType*
ves_icall_System_Reflection_Assembly_InternalGetType (MonoReflectionAssembly *assembly, MonoReflectionModule *module, MonoString *name, MonoBoolean throwOnError, MonoBoolean ignoreCase)
{
	gchar *str;
	MonoType *type = NULL;
	MonoTypeNameParse info;

	MONO_ARCH_SAVE_REGS;

	str = mono_string_to_utf8 (name);
	/*g_print ("requested type %s in %s\n", str, assembly->assembly->aname.name);*/
	if (!mono_reflection_parse_type (str, &info)) {
		g_free (str);
		g_list_free (info.modifiers);
		g_list_free (info.nested);
		if (throwOnError) /* uhm: this is a parse error, though... */
			mono_raise_exception (mono_get_exception_type_load (name));
		/*g_print ("failed parse\n");*/
		return NULL;
	}

	if (module != NULL) {
		if (module->image)
			type = mono_reflection_get_type (module->image, &info, ignoreCase);
		else
			type = NULL;
	}
	else
		if (assembly->assembly->dynamic) {
			/* Enumerate all modules */
			MonoReflectionAssemblyBuilder *abuilder = (MonoReflectionAssemblyBuilder*)assembly;
			int i;

			type = NULL;
			if (abuilder->modules) {
				for (i = 0; i < mono_array_length (abuilder->modules); ++i) {
					MonoReflectionModuleBuilder *mb = mono_array_get (abuilder->modules, MonoReflectionModuleBuilder*, i);
					type = mono_reflection_get_type (&mb->dynamic_image->image, &info, ignoreCase);
					if (type)
						break;
				}
			}

			if (!type && abuilder->loaded_modules) {
				for (i = 0; i < mono_array_length (abuilder->loaded_modules); ++i) {
					MonoReflectionModule *mod = mono_array_get (abuilder->loaded_modules, MonoReflectionModule*, i);
					type = mono_reflection_get_type (mod->image, &info, ignoreCase);
					if (type)
						break;
				}
			}
		}
		else
			type = mono_reflection_get_type (assembly->assembly->image, &info, ignoreCase);
	g_free (str);
	g_list_free (info.modifiers);
	g_list_free (info.nested);
	if (!type) {
		if (throwOnError)
			mono_raise_exception (mono_get_exception_type_load (name));
		/* g_print ("failed find\n"); */
		return NULL;
	}
	/* g_print ("got it\n"); */
	return mono_type_get_object (mono_object_domain (assembly), type);

}

static MonoString *
ves_icall_System_Reflection_Assembly_get_code_base (MonoReflectionAssembly *assembly)
{
	MonoDomain *domain = mono_object_domain (assembly); 
	MonoAssembly *mass = assembly->assembly;
	MonoString *res;
	gchar *uri;
	gchar *absolute;
	
	MONO_ARCH_SAVE_REGS;

	absolute = g_build_filename (mass->basedir, mass->image->module_name, NULL);
	uri = g_filename_to_uri (absolute, NULL, NULL);
	res = mono_string_new (domain, uri);
	g_free (uri);
	g_free (absolute);
	return res;
}

static MonoString *
ves_icall_System_Reflection_Assembly_get_location (MonoReflectionAssembly *assembly)
{
	MonoDomain *domain = mono_object_domain (assembly); 
	MonoString *res;
	char *name = g_build_filename (
		assembly->assembly->basedir,
		assembly->assembly->image->module_name, NULL);

	MONO_ARCH_SAVE_REGS;

	res = mono_string_new (domain, name);
	g_free (name);
	return res;
}

static MonoString *
ves_icall_System_Reflection_Assembly_InternalImageRuntimeVersion (MonoReflectionAssembly *assembly)
{
	MonoDomain *domain = mono_object_domain (assembly); 

	MONO_ARCH_SAVE_REGS;

	return mono_string_new (domain, assembly->assembly->image->version);
}

static MonoReflectionMethod*
ves_icall_System_Reflection_Assembly_get_EntryPoint (MonoReflectionAssembly *assembly) 
{
	guint32 token = mono_image_get_entry_point (assembly->assembly->image);

	MONO_ARCH_SAVE_REGS;

	if (!token)
		return NULL;
	return mono_method_get_object (mono_object_domain (assembly), mono_get_method (assembly->assembly->image, token, NULL), NULL);
}

static MonoArray*
ves_icall_System_Reflection_Assembly_GetManifestResourceNames (MonoReflectionAssembly *assembly) 
{
	MonoTableInfo *table = &assembly->assembly->image->tables [MONO_TABLE_MANIFESTRESOURCE];
	MonoArray *result = mono_array_new (mono_object_domain (assembly), mono_defaults.string_class, table->rows);
	int i;
	const char *val;

	MONO_ARCH_SAVE_REGS;

	for (i = 0; i < table->rows; ++i) {
		val = mono_metadata_string_heap (assembly->assembly->image, mono_metadata_decode_row_col (table, i, MONO_MANIFEST_NAME));
		mono_array_set (result, gpointer, i, mono_string_new (mono_object_domain (assembly), val));
	}
	return result;
}

static MonoArray*
ves_icall_System_Reflection_Assembly_GetReferencedAssemblies (MonoReflectionAssembly *assembly) 
{
	static MonoClass *System_Reflection_AssemblyName;
	MonoArray *result;
	MonoAssembly **ptr;
	MonoDomain *domain = mono_object_domain (assembly);
	int i, count = 0;

	MONO_ARCH_SAVE_REGS;

	if (!System_Reflection_AssemblyName)
		System_Reflection_AssemblyName = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "AssemblyName");

	for (ptr = assembly->assembly->image->references; ptr && *ptr; ptr++)
		count++;

	result = mono_array_new (mono_object_domain (assembly), System_Reflection_AssemblyName, count);

	for (i = 0; i < count; i++) {
		MonoAssembly *assem = assembly->assembly->image->references [i];
		MonoReflectionAssemblyName *aname;
		char *codebase, *absolute;

		aname = (MonoReflectionAssemblyName *) mono_object_new (
			domain, System_Reflection_AssemblyName);

		if (strcmp (assem->aname.name, "corlib") == 0)
			aname->name = mono_string_new (domain, "mscorlib");
		else
			aname->name = mono_string_new (domain, assem->aname.name);
		aname->major = assem->aname.major;

		absolute = g_build_filename (assem->basedir, assem->image->module_name, NULL);
		codebase = g_filename_to_uri (absolute, NULL, NULL);
		aname->codebase = mono_string_new (domain, codebase);
		g_free (codebase);
		g_free (absolute);
		mono_array_set (result, gpointer, i, aname);
	}
	return result;
}

typedef struct {
	MonoArray *res;
	int idx;
} NameSpaceInfo;

static void
foreach_namespace (const char* key, gconstpointer val, NameSpaceInfo *info)
{
	MonoString *name = mono_string_new (mono_object_domain (info->res), key);

	mono_array_set (info->res, gpointer, info->idx, name);
	info->idx++;
}

static MonoArray*
ves_icall_System_Reflection_Assembly_GetNamespaces (MonoReflectionAssembly *assembly) 
{
	MonoImage *img = assembly->assembly->image;
	int n;
	MonoArray *res;
	NameSpaceInfo info;
	MonoTableInfo  *t = &img->tables [MONO_TABLE_EXPORTEDTYPE];
	int i;

	MONO_ARCH_SAVE_REGS;

	n = g_hash_table_size (img->name_cache);
	res = mono_array_new (mono_object_domain (assembly), mono_defaults.string_class, n);
	info.res = res;
	info.idx = 0;
	g_hash_table_foreach (img->name_cache, (GHFunc)foreach_namespace, &info);

	/* Add namespaces from the EXPORTEDTYPES table as well */
	if (t->rows) {
		MonoArray *res2;
		GPtrArray *nspaces = g_ptr_array_new ();
		for (i = 0; i < t->rows; ++i) {
			const char *nspace = mono_metadata_string_heap (img, mono_metadata_decode_row_col (t, i, MONO_EXP_TYPE_NAMESPACE));
			if (!g_hash_table_lookup (img->name_cache, nspace)) {
				g_ptr_array_add (nspaces, (char*)nspace);
			}
		}
		if (nspaces->len > 0) {
			res2 = mono_array_new (mono_object_domain (assembly), mono_defaults.string_class, n + nspaces->len);
			memcpy (mono_array_addr (res2, MonoString*, 0),
					mono_array_addr (res, MonoString*, 0),
					n * sizeof (MonoString*));
			for (i = 0; i < nspaces->len; ++i)
				mono_array_set (res2, MonoString*, n + i, 
								mono_string_new (mono_object_domain (assembly),
												 g_ptr_array_index (nspaces, i)));
			res = res2;
		}
		g_ptr_array_free (nspaces, TRUE);
	}

	return res;
}

/* move this in some file in mono/util/ */
static char *
g_concat_dir_and_file (const char *dir, const char *file)
{
	g_return_val_if_fail (dir != NULL, NULL);
	g_return_val_if_fail (file != NULL, NULL);

        /*
	 * If the directory name doesn't have a / on the end, we need
	 * to add one so we get a proper path to the file
	 */
	if (dir [strlen(dir) - 1] != G_DIR_SEPARATOR)
		return g_strconcat (dir, G_DIR_SEPARATOR_S, file, NULL);
	else
		return g_strconcat (dir, file, NULL);
}

static void *
ves_icall_System_Reflection_Assembly_GetManifestResourceInternal (MonoReflectionAssembly *assembly, MonoString *name, gint32 *size, MonoReflectionModule **ref_module) 
{
	char *n = mono_string_to_utf8 (name);
	MonoTableInfo *table = &assembly->assembly->image->tables [MONO_TABLE_MANIFESTRESOURCE];
	guint32 i;
	guint32 cols [MONO_MANIFEST_SIZE];
	guint32 impl, file_idx;
	const char *val;
	MonoImage *module;

	MONO_ARCH_SAVE_REGS;

	for (i = 0; i < table->rows; ++i) {
		mono_metadata_decode_row (table, i, cols, MONO_MANIFEST_SIZE);
		val = mono_metadata_string_heap (assembly->assembly->image, cols [MONO_MANIFEST_NAME]);
		if (strcmp (val, n) == 0)
			break;
	}
	g_free (n);
	if (i == table->rows)
		return NULL;
	/* FIXME */
	impl = cols [MONO_MANIFEST_IMPLEMENTATION];
	if (impl) {
		/*
		 * this code should only be called after obtaining the 
		 * ResourceInfo and handling the other cases.
		 */
		g_assert ((impl & IMPLEMENTATION_MASK) == IMPLEMENTATION_FILE);
		file_idx = impl >> IMPLEMENTATION_BITS;

		module = mono_image_load_file_for_image (assembly->assembly->image, file_idx);
		if (!module)
			return NULL;
	}
	else
		module = assembly->assembly->image;

	*ref_module = mono_module_get_object (mono_domain_get (), module);

	return (void*)mono_image_get_resource (module, cols [MONO_MANIFEST_OFFSET], size);
}

static gboolean
ves_icall_System_Reflection_Assembly_GetManifestResourceInfoInternal (MonoReflectionAssembly *assembly, MonoString *name, MonoManifestResourceInfo *info)
{
	MonoTableInfo *table = &assembly->assembly->image->tables [MONO_TABLE_MANIFESTRESOURCE];
	int i;
	guint32 cols [MONO_MANIFEST_SIZE];
	guint32 file_cols [MONO_FILE_SIZE];
	const char *val;
	char *n;

	MONO_ARCH_SAVE_REGS;

	n = mono_string_to_utf8 (name);
	for (i = 0; i < table->rows; ++i) {
		mono_metadata_decode_row (table, i, cols, MONO_MANIFEST_SIZE);
		val = mono_metadata_string_heap (assembly->assembly->image, cols [MONO_MANIFEST_NAME]);
		if (strcmp (val, n) == 0)
			break;
	}
	g_free (n);
	if (i == table->rows)
		return FALSE;

	if (!cols [MONO_MANIFEST_IMPLEMENTATION]) {
		info->location = RESOURCE_LOCATION_EMBEDDED | RESOURCE_LOCATION_IN_MANIFEST;
	}
	else {
		switch (cols [MONO_MANIFEST_IMPLEMENTATION] & IMPLEMENTATION_MASK) {
		case IMPLEMENTATION_FILE:
			i = cols [MONO_MANIFEST_IMPLEMENTATION] >> IMPLEMENTATION_BITS;
			table = &assembly->assembly->image->tables [MONO_TABLE_FILE];
			mono_metadata_decode_row (table, i - 1, file_cols, MONO_FILE_SIZE);
			val = mono_metadata_string_heap (assembly->assembly->image, file_cols [MONO_FILE_NAME]);
			info->filename = mono_string_new (mono_object_domain (assembly), val);
			if (file_cols [MONO_FILE_FLAGS] && FILE_CONTAINS_NO_METADATA)
				info->location = 0;
			else
				info->location = RESOURCE_LOCATION_EMBEDDED;
			break;

		case IMPLEMENTATION_ASSEMBLYREF:
			i = cols [MONO_MANIFEST_IMPLEMENTATION] >> IMPLEMENTATION_BITS;
			info->assembly = mono_assembly_get_object (mono_domain_get (), assembly->assembly->image->references [i - 1]);

			// Obtain info recursively
			ves_icall_System_Reflection_Assembly_GetManifestResourceInfoInternal (info->assembly, name, info);
			info->location |= RESOURCE_LOCATION_ANOTHER_ASSEMBLY;
			break;

		case IMPLEMENTATION_EXP_TYPE:
			g_assert_not_reached ();
			break;
		}
	}

	return TRUE;
}

static MonoObject*
ves_icall_System_Reflection_Assembly_GetFilesInternal (MonoReflectionAssembly *assembly, MonoString *name) 
{
	MonoTableInfo *table = &assembly->assembly->image->tables [MONO_TABLE_FILE];
	MonoArray *result = NULL;
	int i;
	const char *val;
	char *n;

	MONO_ARCH_SAVE_REGS;

	/* check hash if needed */
	if (name) {
		n = mono_string_to_utf8 (name);
		for (i = 0; i < table->rows; ++i) {
			val = mono_metadata_string_heap (assembly->assembly->image, mono_metadata_decode_row_col (table, i, MONO_FILE_NAME));
			if (strcmp (val, n) == 0) {
				MonoString *fn;
				g_free (n);
				n = g_concat_dir_and_file (assembly->assembly->basedir, val);
				fn = mono_string_new (mono_object_domain (assembly), n);
				g_free (n);
				return (MonoObject*)fn;
			}
		}
		g_free (n);
		return NULL;
	}

	for (i = 0; i < table->rows; ++i) {
		result = mono_array_new (mono_object_domain (assembly), mono_defaults.string_class, table->rows);
		val = mono_metadata_string_heap (assembly->assembly->image, mono_metadata_decode_row_col (table, i, MONO_FILE_NAME));
		n = g_concat_dir_and_file (assembly->assembly->basedir, val);
		mono_array_set (result, gpointer, i, mono_string_new (mono_object_domain (assembly), n));
		g_free (n);
	}
	return (MonoObject*)result;
}

static MonoArray*
ves_icall_System_Reflection_Assembly_GetModulesInternal (MonoReflectionAssembly *assembly)
{
	MonoDomain *domain = mono_domain_get();
	MonoArray *res;
	MonoClass *klass;
	int i, module_count = 0, file_count = 0;
	MonoImage **modules = assembly->assembly->image->modules;
	MonoTableInfo *table;

	if (modules) {
		while (modules[module_count])
			++module_count;
	}

	table = &assembly->assembly->image->tables [MONO_TABLE_FILE];
	file_count = table->rows;

	g_assert( assembly->assembly->image != NULL);
	++module_count;

	klass = mono_class_from_name ( mono_defaults.corlib, "System.Reflection", "Module");
	res = mono_array_new (domain, klass, module_count + file_count);

	mono_array_set (res, gpointer, 0, mono_module_get_object (domain, assembly->assembly->image));
	for ( i = 1; i < module_count; ++i )
		mono_array_set (res, gpointer, i, mono_module_get_object (domain, modules[i]));

	for (i = 0; i < table->rows; ++i)
		mono_array_set (res, gpointer, module_count + i, mono_module_file_get_object (domain, assembly->assembly->image, i));

	return res;
}

static MonoReflectionMethod*
ves_icall_GetCurrentMethod (void) 
{
	MonoMethod *m = mono_method_get_last_managed ();

	MONO_ARCH_SAVE_REGS;

	return mono_method_get_object (mono_domain_get (), m, NULL);
}

static MonoReflectionAssembly*
ves_icall_System_Reflection_Assembly_GetExecutingAssembly (void)
{
	MonoMethod *m = mono_method_get_last_managed ();

	MONO_ARCH_SAVE_REGS;

	return mono_assembly_get_object (mono_domain_get (), m->klass->image->assembly);
}


static gboolean
get_caller (MonoMethod *m, gint32 no, gint32 ilo, gboolean managed, gpointer data)
{
	MonoMethod **dest = data;

	/* skip unmanaged frames */
	if (!managed)
		return FALSE;

	if (m == *dest) {
		*dest = NULL;
		return FALSE;
	}
	if (!(*dest)) {
		*dest = m;
		return TRUE;
	}
	return FALSE;
}

static MonoReflectionAssembly*
ves_icall_System_Reflection_Assembly_GetEntryAssembly (void)
{
	MonoDomain* domain = mono_domain_get ();

	MONO_ARCH_SAVE_REGS;

	if (!domain->entry_assembly)
		domain = mono_root_domain;

	return mono_assembly_get_object (domain, domain->entry_assembly);
}


static MonoReflectionAssembly*
ves_icall_System_Reflection_Assembly_GetCallingAssembly (void)
{
	MonoMethod *m = mono_method_get_last_managed ();
	MonoMethod *dest = m;

	MONO_ARCH_SAVE_REGS;

	mono_stack_walk (get_caller, &dest);
	if (!dest)
		dest = m;
	return mono_assembly_get_object (mono_domain_get (), dest->klass->image->assembly);
}

static MonoString *
ves_icall_System_MonoType_getFullName (MonoReflectionType *object)
{
	MonoDomain *domain = mono_object_domain (object); 
	MonoString *res;
	gchar *name;

	MONO_ARCH_SAVE_REGS;

	name = mono_type_get_name (object->type);
	res = mono_string_new (domain, name);
	g_free (name);

	return res;
}

static void
fill_reflection_assembly_name (MonoDomain *domain, MonoReflectionAssemblyName *aname, MonoAssemblyName *name, const char *absolute)
{
	static MonoMethod *create_culture = NULL;
    gpointer args [1];
	guint32 pkey_len;
	const char *pkey_ptr;
	gchar *codebase;

	MONO_ARCH_SAVE_REGS;

	if (strcmp (name->name, "corlib") == 0)
		aname->name = mono_string_new (domain, "mscorlib");
	else
		aname->name = mono_string_new (domain, name->name);

	aname->major = name->major;
	aname->minor = name->minor;
	aname->build = name->build;
	aname->revision = name->revision;
	aname->hashalg = name->hash_alg;

	codebase = g_filename_to_uri (absolute, NULL, NULL);
	if (codebase) {
		aname->codebase = mono_string_new (domain, codebase);
		g_free (codebase);
	}

	if (!create_culture) {
		MonoMethodDesc *desc = mono_method_desc_new ("System.Globalization.CultureInfo:CreateSpecificCulture(string)", TRUE);
		create_culture = mono_method_desc_search_in_image (desc, mono_defaults.corlib);
		g_assert (create_culture);
		mono_method_desc_free (desc);
	}

	args [0] = mono_string_new (domain, name->culture);
	aname->cultureInfo = 
		mono_runtime_invoke (create_culture, NULL, args, NULL);

	if (name->public_key) {
		pkey_ptr = name->public_key;
		pkey_len = mono_metadata_decode_blob_size (pkey_ptr, &pkey_ptr);

		aname->publicKey = mono_array_new (domain, mono_defaults.byte_class, pkey_len);
		memcpy (mono_array_addr (aname->publicKey, guint8, 0), pkey_ptr, pkey_len);
	}
}

static void
ves_icall_System_Reflection_Assembly_FillName (MonoReflectionAssembly *assembly, MonoReflectionAssemblyName *aname)
{
	gchar *absolute;

	MONO_ARCH_SAVE_REGS;

	absolute = g_build_filename (assembly->assembly->basedir, assembly->assembly->image->module_name, NULL);

	fill_reflection_assembly_name (mono_object_domain (assembly), aname, 
								   &assembly->assembly->aname, absolute);

	g_free (absolute);
}

static void
ves_icall_System_Reflection_Assembly_InternalGetAssemblyName (MonoString *fname, MonoReflectionAssemblyName *aname)
{
	char *filename;
	MonoImageOpenStatus status = MONO_IMAGE_OK;
	gboolean res;
	MonoImage *image;
	MonoAssemblyName name;

	MONO_ARCH_SAVE_REGS;

	filename = mono_string_to_utf8 (fname);

	image = mono_image_open (filename, &status);
	
	if (!image){
		MonoException *exc;

		g_free (filename);
		exc = mono_get_exception_file_not_found (fname);
		mono_raise_exception (exc);
	}

	res = mono_assembly_fill_assembly_name (image, &name);
	if (!res) {
		mono_image_close (image);
		g_free (filename);
		mono_raise_exception (mono_get_exception_argument ("assemblyFile", "The file does not contain a manifest"));
	}

	fill_reflection_assembly_name (mono_domain_get (), aname, &name, filename);

	g_free (filename);
	mono_image_close (image);
}

static MonoArray*
mono_module_get_types (MonoDomain *domain, MonoImage *image, 
					   MonoBoolean exportedOnly)
{
	MonoArray *res;
	MonoClass *klass;
	MonoTableInfo *tdef = &image->tables [MONO_TABLE_TYPEDEF];
	int i, count;
	guint32 attrs, visibility;

	/* we start the count from 1 because we skip the special type <Module> */
	if (exportedOnly) {
		count = 0;
		for (i = 1; i < tdef->rows; ++i) {
			attrs = mono_metadata_decode_row_col (tdef, i, MONO_TYPEDEF_FLAGS);
			visibility = attrs & TYPE_ATTRIBUTE_VISIBILITY_MASK;
			if (visibility == TYPE_ATTRIBUTE_PUBLIC || visibility == TYPE_ATTRIBUTE_NESTED_PUBLIC)
				count++;
		}
	} else {
		count = tdef->rows - 1;
	}
	res = mono_array_new (domain, mono_defaults.monotype_class, count);
	count = 0;
	for (i = 1; i < tdef->rows; ++i) {
		attrs = mono_metadata_decode_row_col (tdef, i, MONO_TYPEDEF_FLAGS);
		visibility = attrs & TYPE_ATTRIBUTE_VISIBILITY_MASK;
		if (!exportedOnly || (visibility == TYPE_ATTRIBUTE_PUBLIC || visibility == TYPE_ATTRIBUTE_NESTED_PUBLIC)) {
			klass = mono_class_get (image, (i + 1) | MONO_TOKEN_TYPE_DEF);
			mono_array_set (res, gpointer, count, mono_type_get_object (domain, &klass->byval_arg));
			count++;
		}
	}
	
	return res;
}

static MonoArray*
ves_icall_System_Reflection_Assembly_GetTypes (MonoReflectionAssembly *assembly, MonoBoolean exportedOnly)
{
	MonoArray *res;
	MonoImage *image = assembly->assembly->image;
	MonoTableInfo *table = &image->tables [MONO_TABLE_FILE];
	MonoDomain *domain;
	int i;

	MONO_ARCH_SAVE_REGS;

	domain = mono_object_domain (assembly);
	res = mono_module_get_types (domain, image, exportedOnly);

	/* Append data from all modules in the assembly */
	for (i = 0; i < table->rows; ++i) {
		if (!(mono_metadata_decode_row_col (table, i, MONO_FILE_FLAGS) & FILE_CONTAINS_NO_METADATA)) {
			MonoImage *loaded_image = mono_assembly_load_module (image->assembly, i + 1);
			if (loaded_image) {
				MonoArray *res2 = mono_module_get_types (domain, loaded_image, exportedOnly);
				/* Append the new types to the end of the array */
				if (mono_array_length (res2) > 0) {
					guint32 len1, len2;
					MonoArray *res3;

					len1 = mono_array_length (res);
					len2 = mono_array_length (res2);
					res3 = mono_array_new (domain, mono_defaults.monotype_class, len1 + len2);
					memcpy (mono_array_addr (res3, MonoReflectionType*, 0),
							mono_array_addr (res, MonoReflectionType*, 0),
							len1 * sizeof (MonoReflectionType*));
					memcpy (mono_array_addr (res3, MonoReflectionType*, len1),
							mono_array_addr (res2, MonoReflectionType*, 0),
							len2 * sizeof (MonoReflectionType*));
					res = res3;
				}
			}
		}
	}		

	return res;
}

static MonoReflectionType*
ves_icall_System_Reflection_Module_GetGlobalType (MonoReflectionModule *module)
{
	MonoDomain *domain = mono_object_domain (module); 
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	g_assert (module->image);
	klass = mono_class_get (module->image, 1 | MONO_TOKEN_TYPE_DEF);
	return mono_type_get_object (domain, &klass->byval_arg);
}

static void
ves_icall_System_Reflection_Module_Close (MonoReflectionModule *module)
{
	if (module->image)
		mono_image_close (module->image);
}

static MonoString*
ves_icall_System_Reflection_Module_GetGuidInternal (MonoReflectionModule *module)
{
	MonoDomain *domain = mono_object_domain (module); 

	MONO_ARCH_SAVE_REGS;

	g_assert (module->image);
	return mono_string_new (domain, module->image->guid);
}

static MonoArray*
ves_icall_System_Reflection_Module_InternalGetTypes (MonoReflectionModule *module)
{
	MONO_ARCH_SAVE_REGS;

	if (!module->image)
		return mono_array_new (mono_object_domain (module), mono_defaults.monotype_class, 0);
	else
		return mono_module_get_types (mono_object_domain (module), module->image, FALSE);
}

static MonoReflectionType*
ves_icall_ModuleBuilder_create_modified_type (MonoReflectionTypeBuilder *tb, MonoString *smodifiers)
{
	MonoClass *klass;
	int isbyref = 0, rank;
	char *str = mono_string_to_utf8 (smodifiers);
	char *p;

	MONO_ARCH_SAVE_REGS;

	klass = mono_class_from_mono_type (tb->type.type);
	p = str;
	/* logic taken from mono_reflection_parse_type(): keep in sync */
	while (*p) {
		switch (*p) {
		case '&':
			if (isbyref) { /* only one level allowed by the spec */
				g_free (str);
				return NULL;
			}
			isbyref = 1;
			p++;
			g_free (str);
			return mono_type_get_object (mono_object_domain (tb), &klass->this_arg);
			break;
		case '*':
			klass = mono_ptr_class_get (&klass->byval_arg);
			mono_class_init (klass);
			p++;
			break;
		case '[':
			rank = 1;
			p++;
			while (*p) {
				if (*p == ']')
					break;
				if (*p == ',')
					rank++;
				else if (*p != '*') { /* '*' means unknown lower bound */
					g_free (str);
					return NULL;
				}
				++p;
			}
			if (*p != ']') {
				g_free (str);
				return NULL;
			}
			p++;
			klass = mono_array_class_get (klass, rank);
			mono_class_init (klass);
			break;
		default:
			break;
		}
	}
	g_free (str);
	return mono_type_get_object (mono_object_domain (tb), &klass->byval_arg);
}

static MonoBoolean
ves_icall_Type_IsArrayImpl (MonoReflectionType *t)
{
	MonoType *type;
	MonoBoolean res;

	MONO_ARCH_SAVE_REGS;

	type = t->type;
	res = !type->byref && (type->type == MONO_TYPE_ARRAY || type->type == MONO_TYPE_SZARRAY);

	return res;
}

static MonoReflectionType *
ves_icall_Type_make_array_type (MonoReflectionType *type, int rank)
{
	MonoClass *klass, *aklass;

	MONO_ARCH_SAVE_REGS;

	klass = mono_class_from_mono_type (type->type);
	aklass = mono_array_class_get (klass, rank);

	return mono_type_get_object (mono_object_domain (type), &aklass->byval_arg);
}

static MonoReflectionType *
ves_icall_Type_make_byref_type (MonoReflectionType *type)
{
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	klass = mono_class_from_mono_type (type->type);

	return mono_type_get_object (mono_object_domain (type), &klass->this_arg);
}

static MonoObject *
ves_icall_System_Delegate_CreateDelegate_internal (MonoReflectionType *type, MonoObject *target,
						   MonoReflectionMethod *info)
{
	MonoClass *delegate_class = mono_class_from_mono_type (type->type);
	MonoObject *delegate;
	gpointer func;

	MONO_ARCH_SAVE_REGS;

	mono_assert (delegate_class->parent == mono_defaults.multicastdelegate_class);

	delegate = mono_object_new (mono_object_domain (type), delegate_class);

	func = mono_compile_method (info->method);

	mono_delegate_ctor (delegate, target, func);

	return delegate;
}

/*
 * Magic number to convert a time which is relative to
 * Jan 1, 1970 into a value which is relative to Jan 1, 0001.
 */
#define	EPOCH_ADJUST	((guint64)62135596800LL)

/*
 * Magic number to convert FILETIME base Jan 1, 1601 to DateTime - base Jan, 1, 0001
 */
#define FILETIME_ADJUST ((guint64)504911232000000000LL)

/*
 * This returns Now in UTC
 */
static gint64
ves_icall_System_DateTime_GetNow (void)
{
#ifdef PLATFORM_WIN32
	SYSTEMTIME st;
	FILETIME ft;
	
	GetLocalTime (&st);
	SystemTimeToFileTime (&st, &ft);
	return (gint64) FILETIME_ADJUST + ((((gint64)ft.dwHighDateTime)<<32) | ft.dwLowDateTime);
#else
	/* FIXME: put this in io-layer and call it GetLocalTime */
	struct timeval tv;
	gint64 res;

	MONO_ARCH_SAVE_REGS;

	if (gettimeofday (&tv, NULL) == 0) {
		res = (((gint64)tv.tv_sec + EPOCH_ADJUST)* 1000000 + tv.tv_usec)*10;
		return res;
	}
	/* fixme: raise exception */
	return 0;
#endif
}

#ifdef PLATFORM_WIN32
/* convert a SYSTEMTIME which is of the form "last thursday in october" to a real date */
static void
convert_to_absolute_date(SYSTEMTIME *date)
{
#define IS_LEAP(y) ((y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0))
	static int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	static int leap_days_in_month[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	/* from the calendar FAQ */
	int a = (14 - date->wMonth) / 12;
	int y = date->wYear - a;
	int m = date->wMonth + 12 * a - 2;
	int d = (1 + y + y/4 - y/100 + y/400 + (31*m)/12) % 7;

	/* d is now the day of the week for the first of the month (0 == Sunday) */

	int day_of_week = date->wDayOfWeek;

	/* set day_in_month to the first day in the month which falls on day_of_week */    
	int day_in_month = 1 + (day_of_week - d);
	if (day_in_month <= 0)
		day_in_month += 7;

	/* wDay is 1 for first weekday in month, 2 for 2nd ... 5 means last - so work that out allowing for days in the month */
	date->wDay = day_in_month + (date->wDay - 1) * 7;
	if (date->wDay > (IS_LEAP(date->wYear) ? leap_days_in_month[date->wMonth - 1] : days_in_month[date->wMonth - 1]))
		date->wDay -= 7;
}
#endif

#ifndef PLATFORM_WIN32
/*
 * Return's the offset from GMT of a local time.
 * 
 *  tm is a local time
 *  t  is the same local time as seconds.
 */
static int 
gmt_offset(struct tm *tm, time_t t)
{
#if defined (HAVE_TM_GMTOFF)
	return tm->tm_gmtoff;
#else
	struct tm g;
	time_t t2;
	g = *gmtime(&t);
	g.tm_isdst = tm->tm_isdst;
	t2 = mktime(&g);
	return (int)difftime(t, t2);
#endif
}
#endif
/*
 * This is heavily based on zdump.c from glibc 2.2.
 *
 *  * data[0]:  start of daylight saving time (in DateTime ticks).
 *  * data[1]:  end of daylight saving time (in DateTime ticks).
 *  * data[2]:  utcoffset (in TimeSpan ticks).
 *  * data[3]:  additional offset when daylight saving (in TimeSpan ticks).
 *  * name[0]:  name of this timezone when not daylight saving.
 *  * name[1]:  name of this timezone when daylight saving.
 *
 *  FIXME: This only works with "standard" Unix dates (years between 1900 and 2100) while
 *         the class library allows years between 1 and 9999.
 *
 *  Returns true on success and zero on failure.
 */
static guint32
ves_icall_System_CurrentTimeZone_GetTimeZoneData (guint32 year, MonoArray **data, MonoArray **names)
{
#ifndef PLATFORM_WIN32
	MonoDomain *domain = mono_domain_get ();
	struct tm start, tt;
	time_t t;

	long int gmtoff;
	int is_daylight = 0, day;
	char tzone [64];

	MONO_ARCH_SAVE_REGS;

	MONO_CHECK_ARG_NULL (data);
	MONO_CHECK_ARG_NULL (names);

	(*data) = mono_array_new (domain, mono_defaults.int64_class, 4);
	(*names) = mono_array_new (domain, mono_defaults.string_class, 2);

	/* 
	 * no info is better than crashing: we'll need our own tz data to make 
	 * this work properly, anyway. The range is reduced to 1970 .. 2037 because
	 * that is what mktime is guaranteed to support (we get into an infinite loop 
	 * otherwise).
	 */
	if ((year < 1970) || (year > 2037)) {
		t = time (NULL);
		tt = *localtime (&t);
		strftime (tzone, sizeof (tzone), "%Z", &tt);
		mono_array_set ((*names), gpointer, 0, mono_string_new (domain, tzone));
		mono_array_set ((*names), gpointer, 1, mono_string_new (domain, tzone));
		return 1;
	}

	memset (&start, 0, sizeof (start));

	start.tm_mday = 1;
	start.tm_year = year-1900;

	t = mktime (&start);
	gmtoff = gmt_offset (&start, t);

	/* For each day of the year, calculate the tm_gmtoff. */
	for (day = 0; day < 365; day++) {

		t += 3600*24;
		tt = *localtime (&t);

		/* Daylight saving starts or ends here. */
		if (gmt_offset (&tt, t) != gmtoff) {
			struct tm tt1;
			time_t t1;

			/* Try to find the exact hour when daylight saving starts/ends. */
			t1 = t;
			do {
				t1 -= 3600;
				tt1 = *localtime (&t1);
			} while (gmt_offset (&tt1, t1) != gmtoff);

			/* Try to find the exact minute when daylight saving starts/ends. */
			do {
				t1 += 60;
				tt1 = *localtime (&t1);
			} while (gmt_offset (&tt1, t1) == gmtoff);
			
			strftime (tzone, sizeof (tzone), "%Z", &tt);
			
			/* Write data, if we're already in daylight saving, we're done. */
			if (is_daylight) {
				mono_array_set ((*names), gpointer, 0, mono_string_new (domain, tzone));
				mono_array_set ((*data), gint64, 1, ((gint64)t1 + EPOCH_ADJUST) * 10000000L);
				return 1;
			} else {
				mono_array_set ((*names), gpointer, 1, mono_string_new (domain, tzone));
				mono_array_set ((*data), gint64, 0, ((gint64)t1 + EPOCH_ADJUST) * 10000000L);
				is_daylight = 1;
			}

			/* This is only set once when we enter daylight saving. */
			mono_array_set ((*data), gint64, 2, (gint64)gmtoff * 10000000L);
			mono_array_set ((*data), gint64, 3, (gint64)(gmt_offset (&tt, t) - gmtoff) * 10000000L);

			gmtoff = gmt_offset (&tt, t);
		}
	}

	if (!is_daylight) {
		strftime (tzone, sizeof (tzone), "%Z", &tt);
		mono_array_set ((*names), gpointer, 0, mono_string_new (domain, tzone));
		mono_array_set ((*names), gpointer, 1, mono_string_new (domain, tzone));
		mono_array_set ((*data), gint64, 0, 0);
		mono_array_set ((*data), gint64, 1, 0);
		mono_array_set ((*data), gint64, 2, (gint64) gmtoff * 10000000L);
		mono_array_set ((*data), gint64, 3, 0);
	}

	return 1;
#else
	MonoDomain *domain = mono_domain_get ();
	TIME_ZONE_INFORMATION tz_info;
	FILETIME ft;
	int i;
	int err, tz_id;

	tz_id = GetTimeZoneInformation (&tz_info);
	if (tz_id == TIME_ZONE_ID_INVALID)
		return 0;

	MONO_CHECK_ARG_NULL (data);
	MONO_CHECK_ARG_NULL (names);

	(*data) = mono_array_new (domain, mono_defaults.int64_class, 4);
	(*names) = mono_array_new (domain, mono_defaults.string_class, 2);

	for (i = 0; i < 32; ++i)
		if (!tz_info.DaylightName [i])
			break;
	mono_array_set ((*names), gpointer, 1, mono_string_new_utf16 (domain, tz_info.DaylightName, i));
	for (i = 0; i < 32; ++i)
		if (!tz_info.StandardName [i])
			break;
	mono_array_set ((*names), gpointer, 0, mono_string_new_utf16 (domain, tz_info.StandardName, i));

	if ((year <= 1601) || (year > 30827)) {
		/*
		 * According to MSDN, the MS time functions can't handle dates outside
		 * this interval.
		 */
		return 1;
	}

	/* even if the timezone has no daylight savings it may have Bias (e.g. GMT+13 it seems) */
	if (tz_id != TIME_ZONE_ID_UNKNOWN) {
		tz_info.StandardDate.wYear = year;
		convert_to_absolute_date(&tz_info.StandardDate);
		err = SystemTimeToFileTime (&tz_info.StandardDate, &ft);
		g_assert(err);
		mono_array_set ((*data), gint64, 1, FILETIME_ADJUST + (((guint64)ft.dwHighDateTime<<32) | ft.dwLowDateTime));
		tz_info.DaylightDate.wYear = year;
		convert_to_absolute_date(&tz_info.DaylightDate);
		err = SystemTimeToFileTime (&tz_info.DaylightDate, &ft);
		g_assert(err);
		mono_array_set ((*data), gint64, 0, FILETIME_ADJUST + (((guint64)ft.dwHighDateTime<<32) | ft.dwLowDateTime));
	}
	mono_array_set ((*data), gint64, 2, (tz_info.Bias + tz_info.StandardBias) * -600000000LL);
	mono_array_set ((*data), gint64, 3, (tz_info.DaylightBias - tz_info.StandardBias) * -600000000LL);

	return 1;
#endif
}

static gpointer
ves_icall_System_Object_obj_address (MonoObject *this) 
{
	MONO_ARCH_SAVE_REGS;

	return this;
}

/* System.Buffer */

static gint32 
ves_icall_System_Buffer_ByteLengthInternal (MonoArray *array) 
{
	MonoClass *klass;
	MonoTypeEnum etype;
	int length, esize;
	int i;

	MONO_ARCH_SAVE_REGS;

	klass = array->obj.vtable->klass;
	etype = klass->element_class->byval_arg.type;
	if (etype < MONO_TYPE_BOOLEAN || etype > MONO_TYPE_R8)
		return -1;

	if (array->bounds == NULL)
		length = array->max_length;
	else {
		length = 1;
		for (i = 0; i < klass->rank; ++ i)
			length *= array->bounds [i].length;
	}

	esize = mono_array_element_size (klass);
	return length * esize;
}

static gint8 
ves_icall_System_Buffer_GetByteInternal (MonoArray *array, gint32 idx) 
{
	MONO_ARCH_SAVE_REGS;

	return mono_array_get (array, gint8, idx);
}

static void 
ves_icall_System_Buffer_SetByteInternal (MonoArray *array, gint32 idx, gint8 value) 
{
	MONO_ARCH_SAVE_REGS;

	mono_array_set (array, gint8, idx, value);
}

static void 
ves_icall_System_Buffer_BlockCopyInternal (MonoArray *src, gint32 src_offset, MonoArray *dest, gint32 dest_offset, gint32 count) 
{
	char *src_buf, *dest_buf;

	MONO_ARCH_SAVE_REGS;

	src_buf = (gint8 *)src->vector + src_offset;
	dest_buf = (gint8 *)dest->vector + dest_offset;

	memcpy (dest_buf, src_buf, count);
}

static MonoObject *
ves_icall_Remoting_RealProxy_GetTransparentProxy (MonoObject *this, MonoString *class_name)
{
	MonoDomain *domain = mono_object_domain (this); 
	MonoObject *res;
	MonoRealProxy *rp = ((MonoRealProxy *)this);
	MonoTransparentProxy *tp;
	MonoType *type;
	MonoClass *klass;

	MONO_ARCH_SAVE_REGS;

	res = mono_object_new (domain, mono_defaults.transparent_proxy_class);
	tp = (MonoTransparentProxy*) res;
	
	tp->rp = rp;
	type = ((MonoReflectionType *)rp->class_to_proxy)->type;
	klass = mono_class_from_mono_type (type);

	tp->custom_type_info = (mono_object_isinst (this, mono_defaults.iremotingtypeinfo_class) != NULL);
	tp->remote_class = mono_remote_class (domain, class_name, klass);
	res->vtable = tp->remote_class->vtable;

	return res;
}

static MonoReflectionType *
ves_icall_Remoting_RealProxy_InternalGetProxyType (MonoTransparentProxy *tp)
{
	return mono_type_get_object (mono_object_domain (tp), &tp->remote_class->proxy_class->byval_arg);
}

/* System.Environment */

static MonoString *
ves_icall_System_Environment_get_MachineName (void)
{
#if defined (PLATFORM_WIN32)
	gunichar2 *buf;
	guint32 len;
	MonoString *result;

	len = MAX_COMPUTERNAME_LENGTH + 1;
	buf = g_new (gunichar2, len);

	result = NULL;
	if (GetComputerName (buf, (PDWORD) &len))
		result = mono_string_new_utf16 (mono_domain_get (), buf, len);

	g_free (buf);
	return result;
#else
	gchar *buf;
	int len;
	MonoString *result;

	MONO_ARCH_SAVE_REGS;

	len = 256;
	buf = g_new (gchar, len);

	result = NULL;
	if (gethostname (buf, len) == 0)
		result = mono_string_new (mono_domain_get (), buf);
	
	g_free (buf);
	return result;
#endif
}

static int
ves_icall_System_Environment_get_Platform (void)
{
	MONO_ARCH_SAVE_REGS;

#if defined (PLATFORM_WIN32)
	/* Win32NT */
	return 2;
#else
	/* Unix */
	return 128;
#endif
}

static MonoString *
ves_icall_System_Environment_get_NewLine (void)
{
	MONO_ARCH_SAVE_REGS;

#if defined (PLATFORM_WIN32)
	return mono_string_new (mono_domain_get (), "\r\n");
#else
	return mono_string_new (mono_domain_get (), "\n");
#endif
}

static MonoString *
ves_icall_System_Environment_GetEnvironmentVariable (MonoString *name)
{
	const gchar *value;
	gchar *utf8_name;

	MONO_ARCH_SAVE_REGS;

	if (name == NULL)
		return NULL;

	utf8_name = mono_string_to_utf8 (name);	/* FIXME: this should be ascii */
	value = g_getenv (utf8_name);
	g_free (utf8_name);

	if (value == 0)
		return NULL;
	
	return mono_string_new (mono_domain_get (), value);
}

/*
 * There is no standard way to get at environ.
 */
#ifndef _MSC_VER
extern
#endif
char **environ;

static MonoArray *
ves_icall_System_Environment_GetEnvironmentVariableNames (void)
{
	MonoArray *names;
	MonoDomain *domain;
	MonoString *str;
	gchar **e, **parts;
	int n;

	MONO_ARCH_SAVE_REGS;

	n = 0;
	for (e = environ; *e != 0; ++ e)
		++ n;

	domain = mono_domain_get ();
	names = mono_array_new (domain, mono_defaults.string_class, n);

	n = 0;
	for (e = environ; *e != 0; ++ e) {
		parts = g_strsplit (*e, "=", 2);
		if (*parts != 0) {
			str = mono_string_new (domain, *parts);
			mono_array_set (names, MonoString *, n, str);
		}

		g_strfreev (parts);

		++ n;
	}

	return names;
}

/*
 * Returns the number of milliseconds elapsed since the system started.
 */
static gint32
ves_icall_System_Environment_get_TickCount (void)
{
#if defined (PLATFORM_WIN32)
	return GetTickCount();
#else
	struct timeval tv;
	struct timezone tz;
	gint32 res;

	MONO_ARCH_SAVE_REGS;

	res = (gint32) gettimeofday (&tv, &tz);

	if (res != -1)
		res = (gint32) ((tv.tv_sec & 0xFFFFF) * 1000 + (tv.tv_usec / 1000));
	return res;
#endif
}


static void
ves_icall_System_Environment_Exit (int result)
{
	MONO_ARCH_SAVE_REGS;

	mono_runtime_quit ();

	/* we may need to do some cleanup here... */
	exit (result);
}

static MonoString*
ves_icall_System_Environment_GetGacPath (void)
{
	return mono_string_new (mono_domain_get (), MONO_ASSEMBLIES);
}

static MonoString*
ves_icall_System_Environment_GetWindowsFolderPath (int folder)
{
#if defined (PLATFORM_WIN32)
	WCHAR path [MAX_PATH];
	/* Create directory if no existing */
	if (SUCCEEDED (SHGetFolderPathW (NULL, folder | CSIDL_FLAG_CREATE, NULL, 0, path))) {
		int len = 0;
		while (path [len])
			++ len;
		return mono_string_new_utf16 (mono_domain_get (), path, len);
	}
#else
	g_warning ("ves_icall_System_Environment_GetWindowsFolderPath should only be called on Windows!");
#endif
	return mono_string_new (mono_domain_get (), "");
}


static const char *encodings [] = {
	(char *) 1,
		"ascii", "us_ascii", "us", "ansi_x3.4_1968",
		"ansi_x3.4_1986", "cp367", "csascii", "ibm367",
		"iso_ir_6", "iso646_us", "iso_646.irv:1991",
	(char *) 2,
		"utf_7", "csunicode11utf7", "unicode_1_1_utf_7",
		"unicode_2_0_utf_7", "x_unicode_1_1_utf_7",
		"x_unicode_2_0_utf_7",
	(char *) 3,
		"utf_8", "unicode_1_1_utf_8", "unicode_2_0_utf_8",
		"x_unicode_1_1_utf_8", "x_unicode_2_0_utf_8",
	(char *) 4,
		"utf_16", "UTF_16LE", "ucs_2", "unicode",
		"iso_10646_ucs2",
	(char *) 5,
		"unicodefffe", "utf_16be",
	(char *) 6,
		"iso_8859_1",
	(char *) 0
};

/*
 * Returns the internal codepage, if the value of "int_code_page" is
 * 1 at entry, and we can not compute a suitable code page number,
 * returns the code page as a string
 */
static MonoString*
ves_icall_System_Text_Encoding_InternalCodePage (gint32 *int_code_page) 
{
	const char *cset;
	char *p;
	char *codepage = NULL;
	int code;
	int want_name = *int_code_page;
	int i;
	
	*int_code_page = -1;
	MONO_ARCH_SAVE_REGS;

	g_get_charset (&cset);
	p = codepage = strdup (cset);
	for (p = codepage; *p; p++){
		if (isascii (*p) && isalpha (*p))
			*p = tolower (*p);
		if (*p == '-')
			*p = '_';
	}
	/* g_print ("charset: %s\n", cset); */
	
	/* handle some common aliases */
	p = encodings [0];
	code = 0;
	for (i = 0; p != 0; ){
		if ((int) p < 7){
			code = (int) p;
			p = encodings [++i];
			continue;
		}
		if (strcmp (p, codepage) == 0){
			*int_code_page = code;
			break;
		}
		p = encodings [++i];
	}
	
	if (p - codepage > 5){
		if (strstr (codepage, "utf_8") != NULL)
			*int_code_page |= 0x10000000;
	}
	free (codepage);
	
	if (want_name && *int_code_page == -1)
		return mono_string_new (mono_domain_get (), cset);
	else
		return NULL;
}

static MonoBoolean
ves_icall_System_Environment_get_HasShutdownStarted (void)
{
	if (mono_runtime_is_shutting_down ())
		return TRUE;

	if (mono_domain_is_unloading (mono_domain_get ()))
		return TRUE;

	return FALSE;
}

static void
ves_icall_MonoMethodMessage_InitMessage (MonoMethodMessage *this, 
					 MonoReflectionMethod *method,
					 MonoArray *out_args)
{
	MONO_ARCH_SAVE_REGS;

	mono_message_init (mono_object_domain (this), this, method, out_args);
}

static MonoBoolean
ves_icall_IsTransparentProxy (MonoObject *proxy)
{
	MONO_ARCH_SAVE_REGS;

	if (!proxy)
		return 0;

	if (proxy->vtable->klass == mono_defaults.transparent_proxy_class)
		return 1;

	return 0;
}

static void
ves_icall_System_Runtime_Activation_ActivationServices_EnableProxyActivation (MonoReflectionType *type, MonoBoolean enable)
{
	MonoClass *klass;
	MonoVTable* vtable;

	MONO_ARCH_SAVE_REGS;

	klass = mono_class_from_mono_type (type->type);
	vtable = mono_class_vtable (mono_domain_get (), klass);

	if (enable) vtable->remote = 1;
	else vtable->remote = 0;
}

static MonoObject *
ves_icall_System_Runtime_Activation_ActivationServices_AllocateUninitializedClassInstance (MonoReflectionType *type)
{
	MonoClass *klass;
	MonoDomain *domain;
	
	MONO_ARCH_SAVE_REGS;

	domain = mono_object_domain (type);
	klass = mono_class_from_mono_type (type->type);

	if (klass->rank >= 1) {
		g_assert (klass->rank == 1);
		return (MonoObject *) mono_array_new (domain, klass->element_class, 0);
	} else {
		// Bypass remoting object creation check
		return mono_object_new_alloc_specific (mono_class_vtable (domain, klass));
	}
}

static MonoString *
ves_icall_System_IO_get_temp_path (void)
{
	MONO_ARCH_SAVE_REGS;

	return mono_string_new (mono_domain_get (), g_get_tmp_dir ());
}

static gpointer
ves_icall_RuntimeMethod_GetFunctionPointer (MonoMethod *method)
{
	MONO_ARCH_SAVE_REGS;

	return mono_compile_method (method);
}

static MonoString *
ves_icall_System_Configuration_DefaultConfig_get_machine_config_path (void)
{
	MonoString *mcpath;
	gchar *path;

	MONO_ARCH_SAVE_REGS;

	path = g_build_path (G_DIR_SEPARATOR_S, mono_get_config_dir (), "mono", "machine.config", NULL);

#if defined (PLATFORM_WIN32)
	/* Avoid mixing '/' and '\\' */
	{
		gint i;
		for (i = strlen (path) - 1; i >= 0; i--)
			if (path [i] == '/')
				path [i] = '\\';
	}
#endif
	mcpath = mono_string_new (mono_domain_get (), path);
	g_free (path);

	return mcpath;
}

static MonoString *
ves_icall_System_Web_Util_ICalls_get_machine_install_dir (void)
{
	MonoString *ipath;
	gchar *path;

	MONO_ARCH_SAVE_REGS;

	path = g_path_get_dirname (mono_get_config_dir ());

#if defined (PLATFORM_WIN32)
	/* Avoid mixing '/' and '\\' */
	{
		gint i;
		for (i = strlen (path) - 1; i >= 0; i--)
			if (path [i] == '/')
				path [i] = '\\';
	}
#endif
	ipath = mono_string_new (mono_domain_get (), path);
	g_free (path);

	return ipath;
}

static void
ves_icall_System_Diagnostics_DefaultTraceListener_WriteWindowsDebugString (MonoString *message)
{
#if defined (PLATFORM_WIN32)
	static void (*output_debug) (gchar *);
	static gboolean tried_loading = FALSE;

	MONO_ARCH_SAVE_REGS;

	if (!tried_loading && output_debug == NULL) {
		GModule *k32;

		tried_loading = TRUE;
		k32 = g_module_open ("kernel32", G_MODULE_BIND_LAZY);
		if (!k32) {
			gchar *error = g_strdup (g_module_error ());
			g_warning ("Failed to load kernel32.dll: %s\n", error);
			g_free (error);
			return;
		}

		g_module_symbol (k32, "OutputDebugStringW", (gpointer *) &output_debug);
		if (!output_debug) {
			gchar *error = g_strdup (g_module_error ());
			g_warning ("Failed to load OutputDebugStringW: %s\n", error);
			g_free (error);
			return;
		}
	}

	if (output_debug == NULL)
		return;
	
	output_debug (mono_string_chars (message));
#else
	g_warning ("WriteWindowsDebugString called and PLATFORM_WIN32 not defined!\n");
#endif
}

/* Only used for value types */
static MonoObject *
ves_icall_System_Activator_CreateInstanceInternal (MonoReflectionType *type)
{
	MonoClass *klass;
	MonoDomain *domain;
	
	MONO_ARCH_SAVE_REGS;

	domain = mono_object_domain (type);
	klass = mono_class_from_mono_type (type->type);

	return mono_object_new (domain, klass);
}

static MonoReflectionMethod *
ves_icall_MonoMethod_get_base_definition (MonoReflectionMethod *m)
{
	MonoClass *klass;
	MonoMethod *method = m->method;
	MonoMethod *result = NULL;

	MONO_ARCH_SAVE_REGS;

	if (!(method->flags & METHOD_ATTRIBUTE_VIRTUAL) ||
	    MONO_CLASS_IS_INTERFACE (method->klass) ||
	    method->flags & METHOD_ATTRIBUTE_NEW_SLOT)
		return m;

	if (method->klass == NULL || (klass = method->klass->parent) == NULL)
		return m;

	if (klass->generic_inst)
		klass = mono_class_from_mono_type (klass->generic_inst->generic_type);

	while (result == NULL && klass != NULL && (klass->vtable_size > method->slot))
	{
		result = klass->vtable [method->slot];
		if (result == NULL) {
			/* It is an abstract method */
			int i;
			for (i=0; i<klass->method.count; i++) {
				if (klass->methods [i]->slot == method->slot) {
					result = klass->methods [i];
					break;
				}
			}
		}
		klass = klass->parent;
	}

	if (result == NULL)
		return m;

	return mono_method_get_object (mono_domain_get (), result, NULL);
}

static void
mono_ArgIterator_Setup (MonoArgIterator *iter, char* argsp, char* start)
{
	MONO_ARCH_SAVE_REGS;

	iter->sig = *(MonoMethodSignature**)argsp;
	
	g_assert (iter->sig->sentinelpos <= iter->sig->param_count);
	g_assert (iter->sig->call_convention == MONO_CALL_VARARG);

	iter->next_arg = 0;
	/* FIXME: it's not documented what start is exactly... */
	iter->args = start? start: argsp + sizeof (gpointer);
	iter->num_args = iter->sig->param_count - iter->sig->sentinelpos;

	// g_print ("sig %p, param_count: %d, sent: %d\n", iter->sig, iter->sig->param_count, iter->sig->sentinelpos);
}

static MonoTypedRef
mono_ArgIterator_IntGetNextArg (MonoArgIterator *iter)
{
	gint i, align, arg_size;
	MonoTypedRef res;
	MONO_ARCH_SAVE_REGS;

	i = iter->sig->sentinelpos + iter->next_arg;

	g_assert (i < iter->sig->param_count);

	res.type = iter->sig->params [i];
	res.klass = mono_class_from_mono_type (res.type);
	/* FIXME: endianess issue... */
	res.value = iter->args;
	arg_size = mono_type_stack_size (res.type, &align);
	iter->args = (char*)iter->args + arg_size;
	iter->next_arg++;

	//g_print ("returning arg %d, type 0x%02x of size %d at %p\n", i, res.type->type, arg_size, res.value);

	return res;
}

static MonoTypedRef
mono_ArgIterator_IntGetNextArgT (MonoArgIterator *iter, MonoType *type)
{
	gint i, align, arg_size;
	MonoTypedRef res;
	MONO_ARCH_SAVE_REGS;

	i = iter->sig->sentinelpos + iter->next_arg;

	g_assert (i < iter->sig->param_count);

	while (i < iter->sig->param_count) {
		if (!mono_metadata_type_equal (type, iter->sig->params [i]))
			continue;
		res.type = iter->sig->params [i];
		res.klass = mono_class_from_mono_type (res.type);
		/* FIXME: endianess issue... */
		res.value = iter->args;
		arg_size = mono_type_stack_size (res.type, &align);
		iter->args = (char*)iter->args + arg_size;
		iter->next_arg++;
		//g_print ("returning arg %d, type 0x%02x of size %d at %p\n", i, res.type->type, arg_size, res.value);
		return res;
	}
	//g_print ("arg type 0x%02x not found\n", res.type->type);

	res.type = NULL;
	res.value = NULL;
	res.klass = NULL;
	return res;
}

static MonoType*
mono_ArgIterator_IntGetNextArgType (MonoArgIterator *iter)
{
	gint i;
	MONO_ARCH_SAVE_REGS;
	
	i = iter->sig->sentinelpos + iter->next_arg;

	g_assert (i < iter->sig->param_count);

	return iter->sig->params [i];
}

static MonoObject*
mono_TypedReference_ToObject (MonoTypedRef tref)
{
	MONO_ARCH_SAVE_REGS;

	if (MONO_TYPE_IS_REFERENCE (tref.type)) {
		MonoObject** objp = tref.value;
		return *objp;
	}

	return mono_value_box (mono_domain_get (), tref.klass, tref.value);
}

static void
prelink_method (MonoMethod *method)
{
	const char *exc_class, *exc_arg;
	if (!(method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL))
		return;
	mono_lookup_pinvoke_call (method, &exc_class, &exc_arg);
	if (exc_class) {
		mono_raise_exception( 
			mono_exception_from_name_msg (mono_defaults.corlib, "System", exc_class, exc_arg ) );
	}
	/* create the wrapper, too? */
}

static void
ves_icall_System_Runtime_InteropServices_Marshal_Prelink (MonoReflectionMethod *method)
{
	MONO_ARCH_SAVE_REGS;
	prelink_method (method->method);
}

static void
ves_icall_System_Runtime_InteropServices_Marshal_PrelinkAll (MonoReflectionType *type)
{
	MonoClass *klass = mono_class_from_mono_type (type->type);
	int i;
	MONO_ARCH_SAVE_REGS;

	mono_class_init (klass);
	for (i = 0; i < klass->method.count; ++i)
		prelink_method (klass->methods [i]);
}

static void
ves_icall_System_Char_GetDataTablePointers (guint8 **category_data, guint8 **numeric_data,
		gdouble **numeric_data_values, guint16 **to_lower_data_low,
		guint16 **to_lower_data_high, guint16 **to_upper_data_low,
		guint16 **to_upper_data_high)
{
	*category_data = CategoryData;
	*numeric_data = NumericData;
	*numeric_data_values = NumericDataValues;
	*to_lower_data_low = ToLowerDataLow;
	*to_lower_data_high = ToLowerDataHigh;
	*to_upper_data_low = ToUpperDataLow;
	*to_upper_data_high = ToUpperDataHigh;
}

/* icall map */
typedef struct {
	const char *method;
	gconstpointer func;
} IcallEntry;

typedef struct {
	const char *klass;
	const IcallEntry *icalls;
	const int size;
} IcallMap;

static const IcallEntry activator_icalls [] = {
	{"CreateInstanceInternal", ves_icall_System_Activator_CreateInstanceInternal}
};
static const IcallEntry appdomain_icalls [] = {
	{"ExecuteAssembly", ves_icall_System_AppDomain_ExecuteAssembly},
	{"GetAssemblies", ves_icall_System_AppDomain_GetAssemblies},
	{"GetData", ves_icall_System_AppDomain_GetData},
	{"InternalGetContext", ves_icall_System_AppDomain_InternalGetContext},
	{"InternalGetDefaultContext", ves_icall_System_AppDomain_InternalGetDefaultContext},
	{"InternalGetProcessGuid", ves_icall_System_AppDomain_InternalGetProcessGuid},
	{"InternalIsFinalizingForUnload", ves_icall_System_AppDomain_InternalIsFinalizingForUnload},
	{"InternalPopDomainRef", ves_icall_System_AppDomain_InternalPopDomainRef},
	{"InternalPushDomainRef", ves_icall_System_AppDomain_InternalPushDomainRef},
	{"InternalPushDomainRefByID", ves_icall_System_AppDomain_InternalPushDomainRefByID},
	{"InternalSetContext", ves_icall_System_AppDomain_InternalSetContext},
	{"InternalSetDomain", ves_icall_System_AppDomain_InternalSetDomain},
	{"InternalSetDomainByID", ves_icall_System_AppDomain_InternalSetDomainByID},
	{"InternalUnload", ves_icall_System_AppDomain_InternalUnload},
	{"LoadAssembly", ves_icall_System_AppDomain_LoadAssembly},
	{"LoadAssemblyRaw", ves_icall_System_AppDomain_LoadAssemblyRaw},
	{"SetData", ves_icall_System_AppDomain_SetData},
	{"createDomain", ves_icall_System_AppDomain_createDomain},
	{"getCurDomain", ves_icall_System_AppDomain_getCurDomain},
	{"getFriendlyName", ves_icall_System_AppDomain_getFriendlyName},
	{"getSetup", ves_icall_System_AppDomain_getSetup}
};

static const IcallEntry appdomainsetup_icalls [] = {
	{"InitAppDomainSetup", ves_icall_System_AppDomainSetup_InitAppDomainSetup}
};

static const IcallEntry argiterator_icalls [] = {
	{"IntGetNextArg()",                  mono_ArgIterator_IntGetNextArg},
	{"IntGetNextArg(intptr)", mono_ArgIterator_IntGetNextArgT},
	{"IntGetNextArgType",                mono_ArgIterator_IntGetNextArgType},
	{"Setup",                            mono_ArgIterator_Setup}
};

static const IcallEntry array_icalls [] = {
	{"Clone",            mono_array_clone},
	{"CreateInstanceImpl",   ves_icall_System_Array_CreateInstanceImpl},
	{"FastCopy",         ves_icall_System_Array_FastCopy},
	{"GetLength",        ves_icall_System_Array_GetLength},
	{"GetLowerBound",    ves_icall_System_Array_GetLowerBound},
	{"GetRank",          ves_icall_System_Array_GetRank},
	{"GetValue",         ves_icall_System_Array_GetValue},
	{"GetValueImpl",     ves_icall_System_Array_GetValueImpl},
	{"SetValue",         ves_icall_System_Array_SetValue},
	{"SetValueImpl",     ves_icall_System_Array_SetValueImpl}
};

static const IcallEntry buffer_icalls [] = {
	{"BlockCopyInternal", ves_icall_System_Buffer_BlockCopyInternal},
	{"ByteLengthInternal", ves_icall_System_Buffer_ByteLengthInternal},
	{"GetByteInternal", ves_icall_System_Buffer_GetByteInternal},
	{"SetByteInternal", ves_icall_System_Buffer_SetByteInternal}
};

static const IcallEntry char_icalls [] = {
	{"GetDataTablePointers", ves_icall_System_Char_GetDataTablePointers},
	{"ToLower(char,System.Globalization.CultureInfo)", ves_icall_System_Char_InternalToLower_Comp},
	{"ToUpper(char,System.Globalization.CultureInfo)", ves_icall_System_Char_InternalToUpper_Comp},
};

static const IcallEntry defaultconf_icalls [] = {
	{"get_machine_config_path", ves_icall_System_Configuration_DefaultConfig_get_machine_config_path}
};

static const IcallEntry timezone_icalls [] = {
	{"GetTimeZoneData", ves_icall_System_CurrentTimeZone_GetTimeZoneData}
};

static const IcallEntry datetime_icalls [] = {
	{"GetNow", ves_icall_System_DateTime_GetNow}
};

static const IcallEntry decimal_icalls [] = {
	{"decimal2Int64", mono_decimal2Int64},
	{"decimal2UInt64", mono_decimal2UInt64},
	{"decimal2double", mono_decimal2double},
	{"decimal2string", mono_decimal2string},
	{"decimalCompare", mono_decimalCompare},
	{"decimalDiv", mono_decimalDiv},
	{"decimalFloorAndTrunc", mono_decimalFloorAndTrunc},
	{"decimalIncr", mono_decimalIncr},
	{"decimalIntDiv", mono_decimalIntDiv},
	{"decimalMult", mono_decimalMult},
	{"decimalRound", mono_decimalRound},
	{"decimalSetExponent", mono_decimalSetExponent},
	{"double2decimal", mono_double2decimal}, /* FIXME: wrong signature. */
	{"string2decimal", mono_string2decimal}
};

static const IcallEntry delegate_icalls [] = {
	{"CreateDelegate_internal", ves_icall_System_Delegate_CreateDelegate_internal}
};

static const IcallEntry tracelist_icalls [] = {
	{"WriteWindowsDebugString", ves_icall_System_Diagnostics_DefaultTraceListener_WriteWindowsDebugString}
};

static const IcallEntry fileversion_icalls [] = {
	{"GetVersionInfo_internal(string)", ves_icall_System_Diagnostics_FileVersionInfo_GetVersionInfo_internal}
};

static const IcallEntry process_icalls [] = {
	{"ExitCode_internal(intptr)", ves_icall_System_Diagnostics_Process_ExitCode_internal},
	{"ExitTime_internal(intptr)", ves_icall_System_Diagnostics_Process_ExitTime_internal},
	{"GetModules_internal()", ves_icall_System_Diagnostics_Process_GetModules_internal},
	{"GetPid_internal()", ves_icall_System_Diagnostics_Process_GetPid_internal},
	{"GetProcess_internal(int)", ves_icall_System_Diagnostics_Process_GetProcess_internal},
	{"GetProcesses_internal()", ves_icall_System_Diagnostics_Process_GetProcesses_internal},
	{"GetWorkingSet_internal(intptr,int&,int&)", ves_icall_System_Diagnostics_Process_GetWorkingSet_internal},
	{"Kill_internal", ves_icall_System_Diagnostics_Process_Kill_internal},
	{"ProcessName_internal(intptr)", ves_icall_System_Diagnostics_Process_ProcessName_internal},
	{"Process_free_internal(intptr)", ves_icall_System_Diagnostics_Process_Process_free_internal},
	{"SetWorkingSet_internal(intptr,int,int,bool)", ves_icall_System_Diagnostics_Process_SetWorkingSet_internal},
	{"StartTime_internal(intptr)", ves_icall_System_Diagnostics_Process_StartTime_internal},
	{"Start_internal(string,string,intptr,intptr,intptr,System.Diagnostics.Process/ProcInfo&)", ves_icall_System_Diagnostics_Process_Start_internal},
	{"WaitForExit_internal(intptr,int)", ves_icall_System_Diagnostics_Process_WaitForExit_internal}
};

static const IcallEntry double_icalls [] = {
	{"AssertEndianity", ves_icall_System_Double_AssertEndianity},
	{"ParseImpl",    mono_double_ParseImpl}
};

static const IcallEntry enum_icalls [] = {
	{"ToObject", ves_icall_System_Enum_ToObject},
	{"get_value", ves_icall_System_Enum_get_value}
};

static const IcallEntry environment_icalls [] = {
	{"Exit", ves_icall_System_Environment_Exit},
	{"GetCommandLineArgs", mono_runtime_get_main_args},
	{"GetEnvironmentVariable", ves_icall_System_Environment_GetEnvironmentVariable},
	{"GetEnvironmentVariableNames", ves_icall_System_Environment_GetEnvironmentVariableNames},
	{"GetMachineConfigPath",	ves_icall_System_Configuration_DefaultConfig_get_machine_config_path},
	{"GetWindowsFolderPath", ves_icall_System_Environment_GetWindowsFolderPath},
	{"get_ExitCode", mono_environment_exitcode_get},
	{"get_HasShutdownStarted", ves_icall_System_Environment_get_HasShutdownStarted},
	{"get_MachineName", ves_icall_System_Environment_get_MachineName},
	{"get_NewLine", ves_icall_System_Environment_get_NewLine},
	{"get_Platform", ves_icall_System_Environment_get_Platform},
	{"get_TickCount", ves_icall_System_Environment_get_TickCount},
	{"get_UserName", ves_icall_System_Environment_get_UserName},
	{"internalGetGacPath", ves_icall_System_Environment_GetGacPath},
	{"set_ExitCode", mono_environment_exitcode_set}
};

static const IcallEntry cultureinfo_icalls [] = {
	{"construct_compareinfo(object,string)", ves_icall_System_Globalization_CompareInfo_construct_compareinfo},
	{"construct_datetime_format", ves_icall_System_Globalization_CultureInfo_construct_datetime_format},
	{"construct_internal_locale(string)", ves_icall_System_Globalization_CultureInfo_construct_internal_locale},
	{"construct_internal_locale_from_current_locale", ves_icall_System_Globalization_CultureInfo_construct_internal_locale_from_current_locale},
	{"construct_internal_locale_from_lcid", ves_icall_System_Globalization_CultureInfo_construct_internal_locale_from_lcid},
	{"construct_internal_locale_from_name", ves_icall_System_Globalization_CultureInfo_construct_internal_locale_from_name},
	{"construct_internal_locale_from_specific_name", ves_icall_System_Globalization_CultureInfo_construct_internal_locale_from_specific_name},
	{"construct_number_format", ves_icall_System_Globalization_CultureInfo_construct_number_format},
	{"internal_get_cultures", ves_icall_System_Globalization_CultureInfo_internal_get_cultures},
	{"internal_is_lcid_neutral", ves_icall_System_Globalization_CultureInfo_internal_is_lcid_neutral}
};

static const IcallEntry compareinfo_icalls [] = {
	{"assign_sortkey(object,string,System.Globalization.CompareOptions)", ves_icall_System_Globalization_CompareInfo_assign_sortkey},
	{"construct_compareinfo(string)", ves_icall_System_Globalization_CompareInfo_construct_compareinfo},
	{"free_internal_collator()", ves_icall_System_Globalization_CompareInfo_free_internal_collator},
	{"internal_compare(string,int,int,string,int,int,System.Globalization.CompareOptions)", ves_icall_System_Globalization_CompareInfo_internal_compare},
	{"internal_index(string,int,int,char,System.Globalization.CompareOptions,bool)", ves_icall_System_Globalization_CompareInfo_internal_index_char},
	{"internal_index(string,int,int,string,System.Globalization.CompareOptions,bool)", ves_icall_System_Globalization_CompareInfo_internal_index}
};

static const IcallEntry gc_icalls [] = {
	{"GetTotalMemory", ves_icall_System_GC_GetTotalMemory},
	{"InternalCollect", ves_icall_System_GC_InternalCollect},
	{"KeepAlive", ves_icall_System_GC_KeepAlive},
	{"ReRegisterForFinalize", ves_icall_System_GC_ReRegisterForFinalize},
	{"SuppressFinalize", ves_icall_System_GC_SuppressFinalize},
	{"WaitForPendingFinalizers", ves_icall_System_GC_WaitForPendingFinalizers}
};

static const IcallEntry famwatcher_icalls [] = {
	{"InternalFAMNextEvent", ves_icall_System_IO_FAMW_InternalFAMNextEvent}
};

static const IcallEntry filewatcher_icalls [] = {
	{"InternalCloseDirectory", ves_icall_System_IO_FSW_CloseDirectory},
	{"InternalOpenDirectory", ves_icall_System_IO_FSW_OpenDirectory},
	{"InternalReadDirectoryChanges", ves_icall_System_IO_FSW_ReadDirectoryChanges},
	{"InternalSupportsFSW", ves_icall_System_IO_FSW_SupportsFSW}
};

static const IcallEntry path_icalls [] = {
	{"get_temp_path", ves_icall_System_IO_get_temp_path}
};

static const IcallEntry monoio_icalls [] = {
	{"BeginRead", ves_icall_System_IO_MonoIO_BeginRead },
	{"BeginWrite", ves_icall_System_IO_MonoIO_BeginWrite },
	{"Close(intptr,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_Close},
	{"CopyFile(string,string,bool,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_CopyFile},
	{"CreateDirectory(string,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_CreateDirectory},
	{"CreatePipe(intptr&,intptr&)", ves_icall_System_IO_MonoIO_CreatePipe},
	{"DeleteFile(string,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_DeleteFile},
	{"FindClose(intptr,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_FindClose},
	{"FindFirstFile(string,System.IO.MonoIOStat&,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_FindFirstFile},
	{"FindNextFile(intptr,System.IO.MonoIOStat&,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_FindNextFile},
	{"Flush(intptr,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_Flush},
	{"GetCurrentDirectory(System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_GetCurrentDirectory},
	{"GetFileAttributes(string,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_GetFileAttributes},
	{"GetFileStat(string,System.IO.MonoIOStat&,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_GetFileStat},
	{"GetFileType(intptr,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_GetFileType},
	{"GetLength(intptr,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_GetLength},
	{"GetSupportsAsync", ves_icall_System_IO_MonoIO_GetSupportsAsync},
	{"GetTempPath(string&)", ves_icall_System_IO_MonoIO_GetTempPath},
	{"MoveFile(string,string,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_MoveFile},
	{"Open(string,System.IO.FileMode,System.IO.FileAccess,System.IO.FileShare,bool,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_Open},
	{"Read(intptr,byte[],int,int,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_Read},
	{"RemoveDirectory(string,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_RemoveDirectory},
	{"Seek(intptr,long,System.IO.SeekOrigin,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_Seek},
	{"SetCurrentDirectory(string,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_SetCurrentDirectory},
	{"SetFileAttributes(string,System.IO.FileAttributes,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_SetFileAttributes},
	{"SetFileTime(intptr,long,long,long,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_SetFileTime},
	{"SetLength(intptr,long,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_SetLength},
	{"Write(intptr,byte[],int,int,System.IO.MonoIOError&)", ves_icall_System_IO_MonoIO_Write},
	{"get_AltDirectorySeparatorChar", ves_icall_System_IO_MonoIO_get_AltDirectorySeparatorChar},
	{"get_ConsoleError", ves_icall_System_IO_MonoIO_get_ConsoleError},
	{"get_ConsoleInput", ves_icall_System_IO_MonoIO_get_ConsoleInput},
	{"get_ConsoleOutput", ves_icall_System_IO_MonoIO_get_ConsoleOutput},
	{"get_DirectorySeparatorChar", ves_icall_System_IO_MonoIO_get_DirectorySeparatorChar},
	{"get_InvalidPathChars", ves_icall_System_IO_MonoIO_get_InvalidPathChars},
	{"get_PathSeparator", ves_icall_System_IO_MonoIO_get_PathSeparator},
	{"get_VolumeSeparatorChar", ves_icall_System_IO_MonoIO_get_VolumeSeparatorChar}
};

static const IcallEntry math_icalls [] = {
	{"Acos", ves_icall_System_Math_Acos},
	{"Asin", ves_icall_System_Math_Asin},
	{"Atan", ves_icall_System_Math_Atan},
	{"Atan2", ves_icall_System_Math_Atan2},
	{"Cos", ves_icall_System_Math_Cos},
	{"Cosh", ves_icall_System_Math_Cosh},
	{"Exp", ves_icall_System_Math_Exp},
	{"Floor", ves_icall_System_Math_Floor},
	{"Log", ves_icall_System_Math_Log},
	{"Log10", ves_icall_System_Math_Log10},
	{"Pow", ves_icall_System_Math_Pow},
	{"Round", ves_icall_System_Math_Round},
	{"Round2", ves_icall_System_Math_Round2},
	{"Sin", ves_icall_System_Math_Sin},
	{"Sinh", ves_icall_System_Math_Sinh},
	{"Sqrt", ves_icall_System_Math_Sqrt},
	{"Tan", ves_icall_System_Math_Tan},
	{"Tanh", ves_icall_System_Math_Tanh}
};

static const IcallEntry customattrs_icalls [] = {
	{"GetCustomAttributes", mono_reflection_get_custom_attrs}
};

static const IcallEntry enuminfo_icalls [] = {
	{"get_enum_info", ves_icall_get_enum_info}
};

static const IcallEntry fieldinfo_icalls [] = {
	{"internal_from_handle", ves_icall_System_Reflection_FieldInfo_internal_from_handle}
};

static const IcallEntry monotype_icalls [] = {
	{"GetArrayRank", ves_icall_MonoType_GetArrayRank},
	{"GetConstructors", ves_icall_Type_GetConstructors_internal},
	{"GetConstructors_internal", ves_icall_Type_GetConstructors_internal},
	{"GetElementType", ves_icall_MonoType_GetElementType},
	{"GetEvents_internal", ves_icall_Type_GetEvents_internal},
	{"GetField", ves_icall_Type_GetField},
	{"GetFields_internal", ves_icall_Type_GetFields_internal},
	{"GetGenericArguments", ves_icall_MonoType_GetGenericArguments},
	{"GetInterfaces", ves_icall_Type_GetInterfaces},
	{"GetMethodsByName", ves_icall_Type_GetMethodsByName},
	{"GetNestedType", ves_icall_Type_GetNestedType},
	{"GetNestedTypes", ves_icall_Type_GetNestedTypes},
	{"GetPropertiesByName", ves_icall_Type_GetPropertiesByName},
	{"InternalGetEvent", ves_icall_MonoType_GetEvent},
	{"IsByRefImpl", ves_icall_type_isbyref},
	{"IsPointerImpl", ves_icall_type_ispointer},
	{"IsPrimitiveImpl", ves_icall_type_isprimitive},
	{"getFullName", ves_icall_System_MonoType_getFullName},
	{"get_Assembly", ves_icall_MonoType_get_Assembly},
	{"get_BaseType", ves_icall_get_type_parent},
	{"get_DeclaringMethod", ves_icall_MonoType_get_DeclaringMethod},
	{"get_DeclaringType", ves_icall_MonoType_get_DeclaringType},
	{"get_HasGenericArguments", ves_icall_MonoType_get_HasGenericArguments},
	{"get_IsGenericParameter", ves_icall_MonoType_get_IsGenericParameter},
	{"get_Module", ves_icall_MonoType_get_Module},
	{"get_Name", ves_icall_MonoType_get_Name},
	{"get_Namespace", ves_icall_MonoType_get_Namespace},
	{"get_UnderlyingSystemType", ves_icall_MonoType_get_UnderlyingSystemType},
	{"get_attributes", ves_icall_get_attributes},
	{"type_from_obj", mono_type_type_from_obj}
};

static const IcallEntry assembly_icalls [] = {
	{"FillName", ves_icall_System_Reflection_Assembly_FillName},
	{"GetCallingAssembly", ves_icall_System_Reflection_Assembly_GetCallingAssembly},
	{"GetEntryAssembly", ves_icall_System_Reflection_Assembly_GetEntryAssembly},
	{"GetExecutingAssembly", ves_icall_System_Reflection_Assembly_GetExecutingAssembly},
	{"GetFilesInternal", ves_icall_System_Reflection_Assembly_GetFilesInternal},
	{"GetManifestResourceInfoInternal", ves_icall_System_Reflection_Assembly_GetManifestResourceInfoInternal},
	{"GetManifestResourceInternal", ves_icall_System_Reflection_Assembly_GetManifestResourceInternal},
	{"GetManifestResourceNames", ves_icall_System_Reflection_Assembly_GetManifestResourceNames},
	{"GetModulesInternal", ves_icall_System_Reflection_Assembly_GetModulesInternal},
	{"GetNamespaces", ves_icall_System_Reflection_Assembly_GetNamespaces},
	{"GetReferencedAssemblies", ves_icall_System_Reflection_Assembly_GetReferencedAssemblies},
	{"GetTypes", ves_icall_System_Reflection_Assembly_GetTypes},
	{"InternalGetAssemblyName", ves_icall_System_Reflection_Assembly_InternalGetAssemblyName},
	{"InternalGetType", ves_icall_System_Reflection_Assembly_InternalGetType},
	{"InternalImageRuntimeVersion", ves_icall_System_Reflection_Assembly_InternalImageRuntimeVersion},
	{"LoadFrom", ves_icall_System_Reflection_Assembly_LoadFrom},
	/*
	 * Private icalls for the Mono Debugger
	 */
	{"MonoDebugger_GetLocalTypeFromSignature", ves_icall_MonoDebugger_GetLocalTypeFromSignature},
	{"MonoDebugger_GetMethod", ves_icall_MonoDebugger_GetMethod},
	{"MonoDebugger_GetMethodToken", ves_icall_MonoDebugger_GetMethodToken},
	{"MonoDebugger_GetType", ves_icall_MonoDebugger_GetType},
	/* normal icalls again */
	{"get_EntryPoint", ves_icall_System_Reflection_Assembly_get_EntryPoint},
	{"get_code_base", ves_icall_System_Reflection_Assembly_get_code_base},
	{"get_location", ves_icall_System_Reflection_Assembly_get_location}
};

static const IcallEntry methodbase_icalls [] = {
	{"GetCurrentMethod", ves_icall_GetCurrentMethod}
};

static const IcallEntry module_icalls [] = {
	{"Close", ves_icall_System_Reflection_Module_Close},
	{"GetGlobalType", ves_icall_System_Reflection_Module_GetGlobalType},
	{"GetGuidInternal", ves_icall_System_Reflection_Module_GetGuidInternal},
	{"InternalGetTypes", ves_icall_System_Reflection_Module_InternalGetTypes}
};

static const IcallEntry monocmethod_icalls [] = {
  	{"GetGenericMethodDefinition_impl", ves_icall_MonoMethod_GetGenericMethodDefinition},
	{"InternalInvoke", ves_icall_InternalInvoke},
	{"get_Mono_IsInflatedMethod", ves_icall_MonoMethod_get_Mono_IsInflatedMethod}
};

static const IcallEntry monoeventinfo_icalls [] = {
	{"get_event_info", ves_icall_get_event_info}
};

static const IcallEntry monofield_icalls [] = {
	{"GetParentType", ves_icall_MonoField_GetParentType},
	{"GetValueInternal", ves_icall_MonoField_GetValueInternal},
	{"SetValueInternal", ves_icall_FieldInfo_SetValueInternal}
};

static const IcallEntry monogenericinst_icalls [] = {
	{"GetConstructors_internal", ves_icall_MonoGenericInst_GetConstructors},
	{"GetEvents_internal", ves_icall_MonoGenericInst_GetEvents},
	{"GetFields_internal", ves_icall_MonoGenericInst_GetFields},
	{"GetInterfaces_internal", ves_icall_MonoGenericInst_GetInterfaces},
	{"GetMethods_internal", ves_icall_MonoGenericInst_GetMethods},
	{"GetParentType", ves_icall_MonoGenericInst_GetParentType},
	{"GetProperties_internal", ves_icall_MonoGenericInst_GetProperties},
	{"initialize", mono_reflection_generic_inst_initialize}
};

static const IcallEntry generictypeparambuilder_icalls [] = {
	{"initialize", mono_reflection_initialize_generic_parameter}
};

static const IcallEntry monomethod_icalls [] = {
	{"BindGenericParameters", mono_reflection_bind_generic_method_parameters},
	{"GetGenericArguments", ves_icall_MonoMethod_GetGenericArguments},
  	{"GetGenericMethodDefinition_impl", ves_icall_MonoMethod_GetGenericMethodDefinition},
	{"InternalInvoke", ves_icall_InternalInvoke},
	{"get_HasGenericParameters", ves_icall_MonoMethod_get_HasGenericParameters},
	{"get_IsGenericMethodDefinition", ves_icall_MonoMethod_get_IsGenericMethodDefinition},
	{"get_Mono_IsInflatedMethod", ves_icall_MonoMethod_get_Mono_IsInflatedMethod},
	{"get_base_definition", ves_icall_MonoMethod_get_base_definition}
};

static const IcallEntry monomethodinfo_icalls [] = {
	{"get_method_info", ves_icall_get_method_info},
	{"get_parameter_info", ves_icall_get_parameter_info}
};

static const IcallEntry monopropertyinfo_icalls [] = {
	{"get_property_info", ves_icall_get_property_info}
};

static const IcallEntry dns_icalls [] = {
	{"GetHostByAddr_internal(string,string&,string[]&,string[]&)", ves_icall_System_Net_Dns_GetHostByAddr_internal},
	{"GetHostByName_internal(string,string&,string[]&,string[]&)", ves_icall_System_Net_Dns_GetHostByName_internal},
	{"GetHostName_internal(string&)", ves_icall_System_Net_Dns_GetHostName_internal}
};

static const IcallEntry socket_icalls [] = {
	{"Accept_internal(intptr,int&)", ves_icall_System_Net_Sockets_Socket_Accept_internal},
	{"Available_internal(intptr,int&)", ves_icall_System_Net_Sockets_Socket_Available_internal},
	{"Bind_internal(intptr,System.Net.SocketAddress,int&)", ves_icall_System_Net_Sockets_Socket_Bind_internal},
	{"Blocking_internal(intptr,bool,int&)", ves_icall_System_Net_Sockets_Socket_Blocking_internal},
	{"Close_internal(intptr,int&)", ves_icall_System_Net_Sockets_Socket_Close_internal},
	{"Connect_internal(intptr,System.Net.SocketAddress,int&)", ves_icall_System_Net_Sockets_Socket_Connect_internal},
	{"GetSocketOption_arr_internal(intptr,System.Net.Sockets.SocketOptionLevel,System.Net.Sockets.SocketOptionName,byte[]&,int&)", ves_icall_System_Net_Sockets_Socket_GetSocketOption_arr_internal},
	{"GetSocketOption_obj_internal(intptr,System.Net.Sockets.SocketOptionLevel,System.Net.Sockets.SocketOptionName,object&,int&)", ves_icall_System_Net_Sockets_Socket_GetSocketOption_obj_internal},
	{"Listen_internal(intptr,int,int&)", ves_icall_System_Net_Sockets_Socket_Listen_internal},
	{"LocalEndPoint_internal(intptr,int&)", ves_icall_System_Net_Sockets_Socket_LocalEndPoint_internal},
	{"Receive_internal(intptr,byte[],int,int,System.Net.Sockets.SocketFlags,int&)", ves_icall_System_Net_Sockets_Socket_Receive_internal},
	{"RecvFrom_internal(intptr,byte[],int,int,System.Net.Sockets.SocketFlags,System.Net.SocketAddress&,int&)", ves_icall_System_Net_Sockets_Socket_RecvFrom_internal},
	{"RemoteEndPoint_internal(intptr,int&)", ves_icall_System_Net_Sockets_Socket_RemoteEndPoint_internal},
	{"Select_internal(System.Net.Sockets.Socket[]&,System.Net.Sockets.Socket[]&,System.Net.Sockets.Socket[]&,int,int&)", ves_icall_System_Net_Sockets_Socket_Select_internal},
	{"SendTo_internal(intptr,byte[],int,int,System.Net.Sockets.SocketFlags,System.Net.SocketAddress,int&)", ves_icall_System_Net_Sockets_Socket_SendTo_internal},
	{"Send_internal(intptr,byte[],int,int,System.Net.Sockets.SocketFlags,int&)", ves_icall_System_Net_Sockets_Socket_Send_internal},
	{"SetSocketOption_internal(intptr,System.Net.Sockets.SocketOptionLevel,System.Net.Sockets.SocketOptionName,object,byte[],int,int&)", ves_icall_System_Net_Sockets_Socket_SetSocketOption_internal},
	{"Shutdown_internal(intptr,System.Net.Sockets.SocketShutdown,int&)", ves_icall_System_Net_Sockets_Socket_Shutdown_internal},
	{"Socket_internal(System.Net.Sockets.AddressFamily,System.Net.Sockets.SocketType,System.Net.Sockets.ProtocolType,int&)", ves_icall_System_Net_Sockets_Socket_Socket_internal},
	{"WSAIoctl(intptr,int,byte[],byte[],int&)", ves_icall_System_Net_Sockets_Socket_WSAIoctl}
};

static const IcallEntry socketex_icalls [] = {
	{"WSAGetLastError_internal", ves_icall_System_Net_Sockets_SocketException_WSAGetLastError_internal}
};

static const IcallEntry object_icalls [] = {
	{"GetType", ves_icall_System_Object_GetType},
	{"InternalGetHashCode", ves_icall_System_Object_GetHashCode},
	{"MemberwiseClone", ves_icall_System_Object_MemberwiseClone},
	{"obj_address", ves_icall_System_Object_obj_address}
};

static const IcallEntry assemblybuilder_icalls[] = {
	{"InternalAddModule", mono_image_load_module},
	{"basic_init", mono_image_basic_init}
};

static const IcallEntry customattrbuilder_icalls [] = {
	{"GetBlob", mono_reflection_get_custom_attrs_blob}
};

static const IcallEntry dynamicmethod_icalls [] = {
	{"create_dynamic_method", mono_reflection_create_dynamic_method}
};

static const IcallEntry methodbuilder_icalls [] = {
	{"BindGenericParameters", mono_reflection_bind_generic_method_parameters}
};

static const IcallEntry modulebuilder_icalls [] = {
	{"basic_init", mono_image_module_basic_init},
	{"build_metadata", ves_icall_ModuleBuilder_build_metadata},
	{"create_modified_type", ves_icall_ModuleBuilder_create_modified_type},
	{"getDataChunk", ves_icall_ModuleBuilder_getDataChunk},
	{"getToken", ves_icall_ModuleBuilder_getToken},
	{"getUSIndex", mono_image_insert_string}
};

static const IcallEntry signaturehelper_icalls [] = {
	{"get_signature_field", mono_reflection_sighelper_get_signature_field},
	{"get_signature_local", mono_reflection_sighelper_get_signature_local}
};

static const IcallEntry typebuilder_icalls [] = {
	{"create_internal_class", mono_reflection_create_internal_class},
	{"create_runtime_class", mono_reflection_create_runtime_class},
	{"get_IsGenericParameter", ves_icall_TypeBuilder_get_IsGenericParameter},
	{"get_event_info", mono_reflection_event_builder_get_event_info},
	{"setup_generic_class", mono_reflection_setup_generic_class},
	{"setup_internal_class", mono_reflection_setup_internal_class}
};

static const IcallEntry runtimehelpers_icalls [] = {
	{"GetObjectValue", ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_GetObjectValue},
	{"GetOffsetToStringData", ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_GetOffsetToStringData},
	{"InitializeArray", ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_InitializeArray},
	{"RunClassConstructor", ves_icall_System_Runtime_CompilerServices_RuntimeHelpers_RunClassConstructor}
};

static const IcallEntry gchandle_icalls [] = {
	{"FreeHandle", ves_icall_System_GCHandle_FreeHandle},
	{"GetAddrOfPinnedObject", ves_icall_System_GCHandle_GetAddrOfPinnedObject},
	{"GetTarget", ves_icall_System_GCHandle_GetTarget},
	{"GetTargetHandle", ves_icall_System_GCHandle_GetTargetHandle}
};

static const IcallEntry marshal_icalls [] = {
	{"AllocCoTaskMem", ves_icall_System_Runtime_InteropServices_Marshal_AllocCoTaskMem},
	{"AllocHGlobal", mono_marshal_alloc},
	{"DestroyStructure", ves_icall_System_Runtime_InteropServices_Marshal_DestroyStructure},
	{"FreeCoTaskMem", ves_icall_System_Runtime_InteropServices_Marshal_FreeCoTaskMem},
	{"FreeHGlobal", mono_marshal_free},
	{"GetLastWin32Error", ves_icall_System_Runtime_InteropServices_Marshal_GetLastWin32Error},
	{"OffsetOf", ves_icall_System_Runtime_InteropServices_Marshal_OffsetOf},
	{"Prelink", ves_icall_System_Runtime_InteropServices_Marshal_Prelink},
	{"PrelinkAll", ves_icall_System_Runtime_InteropServices_Marshal_PrelinkAll},
	{"PtrToStringAnsi(intptr)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAnsi},
	{"PtrToStringAnsi(intptr,int)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAnsi_len},
	{"PtrToStringAuto(intptr)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAnsi},
	{"PtrToStringAuto(intptr,int)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAnsi_len},
	{"PtrToStringBSTR", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringBSTR},
	{"PtrToStringUni(intptr)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringUni},
	{"PtrToStringUni(intptr,int)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringUni_len},
	{"PtrToStructure(intptr,System.Type)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStructure_type},
	{"PtrToStructure(intptr,object)", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStructure},
	{"ReAllocHGlobal", mono_marshal_realloc},
	{"ReadByte", ves_icall_System_Runtime_InteropServices_Marshal_ReadByte},
	{"ReadInt16", ves_icall_System_Runtime_InteropServices_Marshal_ReadInt16},
	{"ReadInt32", ves_icall_System_Runtime_InteropServices_Marshal_ReadInt32},
	{"ReadInt64", ves_icall_System_Runtime_InteropServices_Marshal_ReadInt64},
	{"ReadIntPtr", ves_icall_System_Runtime_InteropServices_Marshal_ReadIntPtr},
	{"SizeOf", ves_icall_System_Runtime_InteropServices_Marshal_SizeOf},
	{"StringToHGlobalAnsi", ves_icall_System_Runtime_InteropServices_Marshal_StringToHGlobalAnsi},
	{"StringToHGlobalAuto", ves_icall_System_Runtime_InteropServices_Marshal_StringToHGlobalAnsi},
	{"StringToHGlobalUni", ves_icall_System_Runtime_InteropServices_Marshal_StringToHGlobalUni},
	{"StructureToPtr", ves_icall_System_Runtime_InteropServices_Marshal_StructureToPtr},
	{"WriteByte", ves_icall_System_Runtime_InteropServices_Marshal_WriteByte},
	{"WriteInt16", ves_icall_System_Runtime_InteropServices_Marshal_WriteInt16},
	{"WriteInt32", ves_icall_System_Runtime_InteropServices_Marshal_WriteInt32},
	{"WriteInt64", ves_icall_System_Runtime_InteropServices_Marshal_WriteInt64},
	{"WriteIntPtr", ves_icall_System_Runtime_InteropServices_Marshal_WriteIntPtr},
	{"copy_from_unmanaged", ves_icall_System_Runtime_InteropServices_Marshal_copy_from_unmanaged},
	{"copy_to_unmanaged", ves_icall_System_Runtime_InteropServices_Marshal_copy_to_unmanaged}
};

static const IcallEntry activationservices_icalls [] = {
	{"AllocateUninitializedClassInstance", ves_icall_System_Runtime_Activation_ActivationServices_AllocateUninitializedClassInstance},
	{"EnableProxyActivation", ves_icall_System_Runtime_Activation_ActivationServices_EnableProxyActivation}
};

static const IcallEntry monomethodmessage_icalls [] = {
	{"InitMessage", ves_icall_MonoMethodMessage_InitMessage}
};
	
static const IcallEntry realproxy_icalls [] = {
	{"InternalGetProxyType", ves_icall_Remoting_RealProxy_InternalGetProxyType},
	{"InternalGetTransparentProxy", ves_icall_Remoting_RealProxy_GetTransparentProxy}
};

static const IcallEntry remotingservices_icalls [] = {
	{"InternalExecute", ves_icall_InternalExecute},
	{"IsTransparentProxy", ves_icall_IsTransparentProxy}
};

static const IcallEntry rng_icalls [] = {
	{"InternalGetBytes", ves_icall_System_Security_Cryptography_RNGCryptoServiceProvider_InternalGetBytes}
};

static const IcallEntry methodhandle_icalls [] = {
	{"GetFunctionPointer", ves_icall_RuntimeMethod_GetFunctionPointer}
};

static const IcallEntry string_icalls [] = {
	{".ctor(char*)", ves_icall_System_String_ctor_charp},
	{".ctor(char*,int,int)", ves_icall_System_String_ctor_charp_int_int},
	{".ctor(char,int)", ves_icall_System_String_ctor_char_int},
	{".ctor(char[])", ves_icall_System_String_ctor_chara},
	{".ctor(char[],int,int)", ves_icall_System_String_ctor_chara_int_int},
	{".ctor(sbyte*)", ves_icall_System_String_ctor_sbytep},
	{".ctor(sbyte*,int,int)", ves_icall_System_String_ctor_sbytep_int_int},
	{".ctor(sbyte*,int,int,System.Text.Encoding)", ves_icall_System_String_ctor_encoding},
	{"GetHashCode", ves_icall_System_String_GetHashCode},
	{"InternalAllocateStr", ves_icall_System_String_InternalAllocateStr},
	{"InternalCopyTo", ves_icall_System_String_InternalCopyTo},
	{"InternalIndexOfAny", ves_icall_System_String_InternalIndexOfAny},
	{"InternalInsert", ves_icall_System_String_InternalInsert},
	{"InternalIntern", ves_icall_System_String_InternalIntern},
	{"InternalIsInterned", ves_icall_System_String_InternalIsInterned},
	{"InternalJoin", ves_icall_System_String_InternalJoin},
	{"InternalLastIndexOfAny", ves_icall_System_String_InternalLastIndexOfAny},
	{"InternalPad", ves_icall_System_String_InternalPad},
	{"InternalRemove", ves_icall_System_String_InternalRemove},
	{"InternalReplace(char,char)", ves_icall_System_String_InternalReplace_Char},
	{"InternalReplace(string,string,System.Globalization.CompareInfo)", ves_icall_System_String_InternalReplace_Str_Comp},
	{"InternalSplit", ves_icall_System_String_InternalSplit},
	{"InternalStrcpy(string,int,string)", ves_icall_System_String_InternalStrcpy_Str},
	{"InternalStrcpy(string,int,string,int,int)", ves_icall_System_String_InternalStrcpy_StrN},
	{"InternalToLower(System.Globalization.CultureInfo)", ves_icall_System_String_InternalToLower_Comp},
	{"InternalToUpper(System.Globalization.CultureInfo)", ves_icall_System_String_InternalToUpper_Comp},
	{"InternalTrim", ves_icall_System_String_InternalTrim},
	{"get_Chars", ves_icall_System_String_get_Chars}
};

static const IcallEntry encoding_icalls [] = {
	{"InternalCodePage", ves_icall_System_Text_Encoding_InternalCodePage}
};

static const IcallEntry monitor_icalls [] = {
	{"Monitor_exit", ves_icall_System_Threading_Monitor_Monitor_exit},
	{"Monitor_pulse", ves_icall_System_Threading_Monitor_Monitor_pulse},
	{"Monitor_pulse_all", ves_icall_System_Threading_Monitor_Monitor_pulse_all},
	{"Monitor_test_owner", ves_icall_System_Threading_Monitor_Monitor_test_owner},
	{"Monitor_test_synchronised", ves_icall_System_Threading_Monitor_Monitor_test_synchronised},
	{"Monitor_try_enter", ves_icall_System_Threading_Monitor_Monitor_try_enter},
	{"Monitor_wait", ves_icall_System_Threading_Monitor_Monitor_wait}
};

static const IcallEntry interlocked_icalls [] = {
	{"CompareExchange(int&,int,int)", ves_icall_System_Threading_Interlocked_CompareExchange_Int},
	{"CompareExchange(object&,object,object)", ves_icall_System_Threading_Interlocked_CompareExchange_Object},
	{"CompareExchange(single&,single,single)", ves_icall_System_Threading_Interlocked_CompareExchange_Single},
	{"Decrement(int&)", ves_icall_System_Threading_Interlocked_Decrement_Int},
	{"Decrement(long&)", ves_icall_System_Threading_Interlocked_Decrement_Long},
	{"Exchange(int&,int)", ves_icall_System_Threading_Interlocked_Exchange_Int},
	{"Exchange(object&,object)", ves_icall_System_Threading_Interlocked_Exchange_Object},
	{"Exchange(single&,single)", ves_icall_System_Threading_Interlocked_Exchange_Single},
	{"Increment(int&)", ves_icall_System_Threading_Interlocked_Increment_Int},
	{"Increment(long&)", ves_icall_System_Threading_Interlocked_Increment_Long}
};

static const IcallEntry mutex_icalls [] = {
	{"CreateMutex_internal", ves_icall_System_Threading_Mutex_CreateMutex_internal},
	{"ReleaseMutex_internal", ves_icall_System_Threading_Mutex_ReleaseMutex_internal}
};

static const IcallEntry nativeevents_icalls [] = {
	{"CloseEvent_internal", ves_icall_System_Threading_Events_CloseEvent_internal},
	{"CreateEvent_internal", ves_icall_System_Threading_Events_CreateEvent_internal},
	{"ResetEvent_internal",  ves_icall_System_Threading_Events_ResetEvent_internal},
	{"SetEvent_internal",    ves_icall_System_Threading_Events_SetEvent_internal}
};

static const IcallEntry thread_icalls [] = {
	{"Abort_internal(object)", ves_icall_System_Threading_Thread_Abort},
	{"CurrentThread_internal", mono_thread_current},
	{"GetDomainID", ves_icall_System_Threading_Thread_GetDomainID},
	{"GetName_internal", ves_icall_System_Threading_Thread_GetName_internal},
	{"Join_internal", ves_icall_System_Threading_Thread_Join_internal},
	{"ResetAbort_internal()", ves_icall_System_Threading_Thread_ResetAbort},
	{"SetName_internal", ves_icall_System_Threading_Thread_SetName_internal},
	{"Sleep_internal", ves_icall_System_Threading_Thread_Sleep_internal},
	{"SlotHash_lookup", ves_icall_System_Threading_Thread_SlotHash_lookup},
	{"SlotHash_store", ves_icall_System_Threading_Thread_SlotHash_store},
	{"Start_internal", ves_icall_System_Threading_Thread_Start_internal},
	{"Thread_free_internal", ves_icall_System_Threading_Thread_Thread_free_internal},
	{"Thread_internal", ves_icall_System_Threading_Thread_Thread_internal},
	{"VolatileRead(IntPtr&)", ves_icall_System_Threading_Thread_VolatileReadIntPtr},
	{"VolatileRead(UIntPtr&)", ves_icall_System_Threading_Thread_VolatileReadIntPtr},
	{"VolatileRead(byte&)", ves_icall_System_Threading_Thread_VolatileRead1},
	{"VolatileRead(double&)", ves_icall_System_Threading_Thread_VolatileRead8},
	{"VolatileRead(float&)", ves_icall_System_Threading_Thread_VolatileRead4},
	{"VolatileRead(int&)", ves_icall_System_Threading_Thread_VolatileRead4},
	{"VolatileRead(long&)", ves_icall_System_Threading_Thread_VolatileRead8},
	{"VolatileRead(object&)", ves_icall_System_Threading_Thread_VolatileReadIntPtr},
	{"VolatileRead(sbyte&)", ves_icall_System_Threading_Thread_VolatileRead1},
	{"VolatileRead(short&)", ves_icall_System_Threading_Thread_VolatileRead2},
	{"VolatileRead(uint&)", ves_icall_System_Threading_Thread_VolatileRead2},
	{"VolatileRead(ulong&)", ves_icall_System_Threading_Thread_VolatileRead8},
	{"VolatileRead(ushort&)", ves_icall_System_Threading_Thread_VolatileRead2},
	{"VolatileWrite(IntPtr&,IntPtr)", ves_icall_System_Threading_Thread_VolatileWriteIntPtr},
	{"VolatileWrite(UIntPtr&,UIntPtr)", ves_icall_System_Threading_Thread_VolatileWriteIntPtr},
	{"VolatileWrite(byte&,byte)", ves_icall_System_Threading_Thread_VolatileWrite1},
	{"VolatileWrite(double&,double)", ves_icall_System_Threading_Thread_VolatileWrite8},
	{"VolatileWrite(float&,float)", ves_icall_System_Threading_Thread_VolatileWrite4},
	{"VolatileWrite(int&,int)", ves_icall_System_Threading_Thread_VolatileWrite4},
	{"VolatileWrite(long&,long)", ves_icall_System_Threading_Thread_VolatileWrite8},
	{"VolatileWrite(object&,object)", ves_icall_System_Threading_Thread_VolatileWriteIntPtr},
	{"VolatileWrite(sbyte&,sbyte)", ves_icall_System_Threading_Thread_VolatileWrite1},
	{"VolatileWrite(short&,short)", ves_icall_System_Threading_Thread_VolatileWrite2},
	{"VolatileWrite(uint&,uint)", ves_icall_System_Threading_Thread_VolatileWrite2},
	{"VolatileWrite(ulong&,ulong)", ves_icall_System_Threading_Thread_VolatileWrite8},
	{"VolatileWrite(ushort&,ushort)", ves_icall_System_Threading_Thread_VolatileWrite2},
	{"current_lcid()", ves_icall_System_Threading_Thread_current_lcid}
};

static const IcallEntry threadpool_icalls [] = {
	{"BindHandleInternal", ves_icall_System_Threading_ThreadPool_BindHandle},
	{"GetAvailableThreads", ves_icall_System_Threading_ThreadPool_GetAvailableThreads},
	{"GetMaxThreads", ves_icall_System_Threading_ThreadPool_GetMaxThreads},
	{"GetMinThreads", ves_icall_System_Threading_ThreadPool_GetMinThreads},
	{"SetMinThreads", ves_icall_System_Threading_ThreadPool_SetMinThreads}
};

static const IcallEntry waithandle_icalls [] = {
	{"WaitAll_internal", ves_icall_System_Threading_WaitHandle_WaitAll_internal},
	{"WaitAny_internal", ves_icall_System_Threading_WaitHandle_WaitAny_internal},
	{"WaitOne_internal", ves_icall_System_Threading_WaitHandle_WaitOne_internal}
};

static const IcallEntry type_icalls [] = {
	{"BindGenericParameters", ves_icall_Type_BindGenericParameters},
	{"Equals", ves_icall_type_Equals},
	{"GetGenericParameterPosition", ves_icall_Type_GetGenericParameterPosition},
	{"GetGenericTypeDefinition_impl", ves_icall_Type_GetGenericTypeDefinition_impl},
	{"GetInterfaceMapData", ves_icall_Type_GetInterfaceMapData},
	{"GetTypeCode", ves_icall_type_GetTypeCode},
	{"IsArrayImpl", ves_icall_Type_IsArrayImpl},
	{"IsInstanceOfType", ves_icall_type_IsInstanceOfType},
	{"get_IsGenericInstance", ves_icall_Type_get_IsGenericInstance},
	{"get_IsGenericTypeDefinition", ves_icall_Type_get_IsGenericTypeDefinition},
	{"internal_from_handle", ves_icall_type_from_handle},
	{"internal_from_name", ves_icall_type_from_name},
	{"make_array_type", ves_icall_Type_make_array_type},
	{"make_byref_type", ves_icall_Type_make_byref_type},
	{"type_is_assignable_from", ves_icall_type_is_assignable_from},
	{"type_is_subtype_of", ves_icall_type_is_subtype_of}
};

static const IcallEntry typedref_icalls [] = {
	{"ToObject",	mono_TypedReference_ToObject}
};

static const IcallEntry valuetype_icalls [] = {
	{"InternalEquals", ves_icall_System_ValueType_Equals},
	{"InternalGetHashCode", ves_icall_System_ValueType_InternalGetHashCode}
};

static const IcallEntry web_icalls [] = {
	{"GetMachineConfigPath", ves_icall_System_Configuration_DefaultConfig_get_machine_config_path},
	{"GetMachineInstallDirectory", ves_icall_System_Web_Util_ICalls_get_machine_install_dir}
};

static const IcallEntry identity_icalls [] = {
	{"GetCurrentToken", ves_icall_System_Security_Principal_WindowsIdentity_GetCurrentToken},
	{"GetTokenName", ves_icall_System_Security_Principal_WindowsIdentity_GetTokenName},
	{"GetUserToken", ves_icall_System_Security_Principal_WindowsIdentity_GetUserToken},
	{"_GetRoles", ves_icall_System_Security_Principal_WindowsIdentity_GetRoles}
};

static const IcallEntry impersonation_icalls [] = {
	{"CloseToken", ves_icall_System_Security_Principal_WindowsImpersonationContext_CloseToken},
	{"DuplicateToken", ves_icall_System_Security_Principal_WindowsImpersonationContext_DuplicateToken},
	{"RevertToSelf", ves_icall_System_Security_Principal_WindowsImpersonationContext_RevertToSelf},
	{"SetCurrentToken", ves_icall_System_Security_Principal_WindowsImpersonationContext_SetCurrentToken}
};

static const IcallEntry principal_icalls [] = {
	{"IsMemberOfGroupId", ves_icall_System_Security_Principal_WindowsPrincipal_IsMemberOfGroupId},
	{"IsMemberOfGroupName", ves_icall_System_Security_Principal_WindowsPrincipal_IsMemberOfGroupName}
};

static const IcallEntry keypair_icalls [] = {
	{"_CanSecure", ves_icall_Mono_Security_Cryptography_KeyPairPersistence_CanSecure},
	{"_IsMachineProtected", ves_icall_Mono_Security_Cryptography_KeyPairPersistence_IsMachineProtected},
	{"_IsUserProtected", ves_icall_Mono_Security_Cryptography_KeyPairPersistence_IsUserProtected},
	{"_ProtectMachine", ves_icall_Mono_Security_Cryptography_KeyPairPersistence_ProtectMachine},
	{"_ProtectUser", ves_icall_Mono_Security_Cryptography_KeyPairPersistence_ProtectUser}
};

/* proto
static const IcallEntry array_icalls [] = {
};

*/

/* keep the entries all sorted */
static const IcallMap icall_entries [] = {
	{"Mono.Security.Cryptography.KeyPairPersistence", keypair_icalls, G_N_ELEMENTS (keypair_icalls)},
	{"System.Activator", activator_icalls, G_N_ELEMENTS (activator_icalls)},
	{"System.AppDomain", appdomain_icalls, G_N_ELEMENTS (appdomain_icalls)},
	{"System.AppDomainSetup", appdomainsetup_icalls, G_N_ELEMENTS (appdomainsetup_icalls)},
	{"System.ArgIterator", argiterator_icalls, G_N_ELEMENTS (argiterator_icalls)},
	{"System.Array", array_icalls, G_N_ELEMENTS (array_icalls)},
	{"System.Buffer", buffer_icalls, G_N_ELEMENTS (buffer_icalls)},
	{"System.Char", char_icalls, G_N_ELEMENTS (char_icalls)},
	{"System.Configuration.DefaultConfig", defaultconf_icalls, G_N_ELEMENTS (defaultconf_icalls)},
	{"System.CurrentTimeZone", timezone_icalls, G_N_ELEMENTS (timezone_icalls)},
	{"System.DateTime", datetime_icalls, G_N_ELEMENTS (datetime_icalls)},
	{"System.Decimal", decimal_icalls, G_N_ELEMENTS (decimal_icalls)},
	{"System.Delegate", delegate_icalls, G_N_ELEMENTS (delegate_icalls)},
	{"System.Diagnostics.DefaultTraceListener", tracelist_icalls, G_N_ELEMENTS (tracelist_icalls)},
	{"System.Diagnostics.FileVersionInfo", fileversion_icalls, G_N_ELEMENTS (fileversion_icalls)},
	{"System.Diagnostics.Process", process_icalls, G_N_ELEMENTS (process_icalls)},
	{"System.Double", double_icalls, G_N_ELEMENTS (double_icalls)},
	{"System.Enum", enum_icalls, G_N_ELEMENTS (enum_icalls)},
	{"System.Environment", environment_icalls, G_N_ELEMENTS (environment_icalls)},
	{"System.GC", gc_icalls, G_N_ELEMENTS (gc_icalls)},
	{"System.Globalization.CompareInfo", compareinfo_icalls, G_N_ELEMENTS (compareinfo_icalls)},
	{"System.Globalization.CultureInfo", cultureinfo_icalls, G_N_ELEMENTS (cultureinfo_icalls)},
	{"System.IO.FAMWatcher", famwatcher_icalls, G_N_ELEMENTS (famwatcher_icalls)},
	{"System.IO.FileSystemWatcher", filewatcher_icalls, G_N_ELEMENTS (filewatcher_icalls)},
	{"System.IO.MonoIO", monoio_icalls, G_N_ELEMENTS (monoio_icalls)},
	{"System.IO.Path", path_icalls, G_N_ELEMENTS (path_icalls)},
	{"System.Math", math_icalls, G_N_ELEMENTS (math_icalls)},
	{"System.MonoCustomAttrs", customattrs_icalls, G_N_ELEMENTS (customattrs_icalls)},
	{"System.MonoEnumInfo", enuminfo_icalls, G_N_ELEMENTS (enuminfo_icalls)},
	{"System.MonoType", monotype_icalls, G_N_ELEMENTS (monotype_icalls)},
	{"System.Net.Dns", dns_icalls, G_N_ELEMENTS (dns_icalls)},
	{"System.Net.Sockets.Socket", socket_icalls, G_N_ELEMENTS (socket_icalls)},
	{"System.Net.Sockets.SocketException", socketex_icalls, G_N_ELEMENTS (socketex_icalls)},
	{"System.Object", object_icalls, G_N_ELEMENTS (object_icalls)},
	{"System.Reflection.Assembly", assembly_icalls, G_N_ELEMENTS (assembly_icalls)},
	{"System.Reflection.Emit.AssemblyBuilder", assemblybuilder_icalls, G_N_ELEMENTS (assemblybuilder_icalls)},
	{"System.Reflection.Emit.CustomAttributeBuilder", customattrbuilder_icalls, G_N_ELEMENTS (customattrbuilder_icalls)},
	{"System.Reflection.Emit.DynamicMethod", dynamicmethod_icalls, G_N_ELEMENTS (dynamicmethod_icalls)},
	{"System.Reflection.Emit.GenericTypeParameterBuilder", generictypeparambuilder_icalls, G_N_ELEMENTS (generictypeparambuilder_icalls)},
	{"System.Reflection.Emit.MethodBuilder", methodbuilder_icalls, G_N_ELEMENTS (methodbuilder_icalls)},
	{"System.Reflection.Emit.ModuleBuilder", modulebuilder_icalls, G_N_ELEMENTS (modulebuilder_icalls)},
	{"System.Reflection.Emit.SignatureHelper", signaturehelper_icalls, G_N_ELEMENTS (signaturehelper_icalls)},
	{"System.Reflection.Emit.TypeBuilder", typebuilder_icalls, G_N_ELEMENTS (typebuilder_icalls)},
	{"System.Reflection.FieldInfo", fieldinfo_icalls, G_N_ELEMENTS (fieldinfo_icalls)},
	{"System.Reflection.MethodBase", methodbase_icalls, G_N_ELEMENTS (methodbase_icalls)},
	{"System.Reflection.Module", module_icalls, G_N_ELEMENTS (module_icalls)},
	{"System.Reflection.MonoCMethod", monocmethod_icalls, G_N_ELEMENTS (monocmethod_icalls)},
	{"System.Reflection.MonoEventInfo", monoeventinfo_icalls, G_N_ELEMENTS (monoeventinfo_icalls)},
	{"System.Reflection.MonoField", monofield_icalls, G_N_ELEMENTS (monofield_icalls)},
	{"System.Reflection.MonoGenericInst", monogenericinst_icalls, G_N_ELEMENTS (monogenericinst_icalls)},
	{"System.Reflection.MonoMethod", monomethod_icalls, G_N_ELEMENTS (monomethod_icalls)},
	{"System.Reflection.MonoMethodInfo", monomethodinfo_icalls, G_N_ELEMENTS (monomethodinfo_icalls)},
	{"System.Reflection.MonoPropertyInfo", monopropertyinfo_icalls, G_N_ELEMENTS (monopropertyinfo_icalls)},
	{"System.Runtime.CompilerServices.RuntimeHelpers", runtimehelpers_icalls, G_N_ELEMENTS (runtimehelpers_icalls)},
	{"System.Runtime.InteropServices.GCHandle", gchandle_icalls, G_N_ELEMENTS (gchandle_icalls)},
	{"System.Runtime.InteropServices.Marshal", marshal_icalls, G_N_ELEMENTS (marshal_icalls)},
	{"System.Runtime.Remoting.Activation.ActivationServices", activationservices_icalls, G_N_ELEMENTS (activationservices_icalls)},
	{"System.Runtime.Remoting.Messaging.MonoMethodMessage", monomethodmessage_icalls, G_N_ELEMENTS (monomethodmessage_icalls)},
	{"System.Runtime.Remoting.Proxies.RealProxy", realproxy_icalls, G_N_ELEMENTS (realproxy_icalls)},
	{"System.Runtime.Remoting.RemotingServices", remotingservices_icalls, G_N_ELEMENTS (remotingservices_icalls)},
	{"System.RuntimeMethodHandle", methodhandle_icalls, G_N_ELEMENTS (methodhandle_icalls)},
	{"System.Security.Cryptography.RNGCryptoServiceProvider", rng_icalls, G_N_ELEMENTS (rng_icalls)},
	{"System.Security.Principal.WindowsIdentity", identity_icalls, G_N_ELEMENTS (identity_icalls)},
	{"System.Security.Principal.WindowsImpersonationContext", impersonation_icalls, G_N_ELEMENTS (impersonation_icalls)},
	{"System.Security.Principal.WindowsPrincipal", principal_icalls, G_N_ELEMENTS (principal_icalls)},
	{"System.String", string_icalls, G_N_ELEMENTS (string_icalls)},
	{"System.Text.Encoding", encoding_icalls, G_N_ELEMENTS (encoding_icalls)},
	{"System.Threading.Interlocked", interlocked_icalls, G_N_ELEMENTS (interlocked_icalls)},
	{"System.Threading.Monitor", monitor_icalls, G_N_ELEMENTS (monitor_icalls)},
	{"System.Threading.Mutex", mutex_icalls, G_N_ELEMENTS (mutex_icalls)},
	{"System.Threading.NativeEventCalls", nativeevents_icalls, G_N_ELEMENTS (nativeevents_icalls)},
	{"System.Threading.Thread", thread_icalls, G_N_ELEMENTS (thread_icalls)},
	{"System.Threading.ThreadPool", threadpool_icalls, G_N_ELEMENTS (threadpool_icalls)},
	{"System.Threading.WaitHandle", waithandle_icalls, G_N_ELEMENTS (waithandle_icalls)},
	{"System.Type", type_icalls, G_N_ELEMENTS (type_icalls)},
	{"System.TypedReference", typedref_icalls, G_N_ELEMENTS (typedref_icalls)},
	{"System.ValueType", valuetype_icalls, G_N_ELEMENTS (valuetype_icalls)},
	{"System.Web.Util.ICalls", web_icalls, G_N_ELEMENTS (web_icalls)}
};

static GHashTable *icall_hash = NULL;

void
mono_init_icall (void)
{
	int i = 0;

	/* check that tables are sorted: disable in release */
	if (TRUE) {
		int j;
		const IcallMap *imap;
		const IcallEntry *ientry;
		const char *prev_class = NULL;
		const char *prev_method;
		
		for (i = 0; i < G_N_ELEMENTS (icall_entries); ++i) {
			imap = &icall_entries [i];
			prev_method = NULL;
			if (prev_class && strcmp (prev_class, imap->klass) >= 0)
				g_print ("class %s should come before class %s\n", imap->klass, prev_class);
			prev_class = imap->klass;
			for (j = 0; j < imap->size; ++j) {
				ientry = &imap->icalls [j];
				if (prev_method && strcmp (prev_method, ientry->method) >= 0)
					g_print ("method %s should come before method %s\n", ientry->method, prev_method);
				prev_method = ientry->method;
			}
		}
	}

	icall_hash = g_hash_table_new (g_str_hash , g_str_equal);
}

void
mono_add_internal_call (const char *name, gconstpointer method)
{
	mono_loader_lock ();

	g_hash_table_insert (icall_hash, g_strdup (name), (gpointer) method);

	mono_loader_unlock ();
}

static int
compare_class_imap (const void *key, const void *elem)
{
	const IcallMap* imap = (const IcallMap*)elem;
	return strcmp (key, imap->klass);
}

static const IcallMap*
find_class_icalls (const char *name)
{
	return (const IcallMap*) bsearch (name, icall_entries, G_N_ELEMENTS (icall_entries), sizeof (IcallMap), compare_class_imap);
}

static int
compare_method_imap (const void *key, const void *elem)
{
	const IcallEntry* ientry = (const IcallEntry*)elem;
	return strcmp (key, ientry->method);
}

static void*
find_method_icall (const IcallMap *imap, const char *name)
{
	const IcallEntry *ientry = (const IcallEntry*) bsearch (name, imap->icalls, imap->size, sizeof (IcallEntry), compare_method_imap);
	if (ientry)
		return (void*)ientry->func;
	return NULL;
}

/* 
 * we should probably export this as an helper (handle nested types).
 * Returns the number of chars written in buf.
 */
static int
concat_class_name (char *buf, int bufsize, MonoClass *klass)
{
	int nspacelen, cnamelen;
	nspacelen = strlen (klass->name_space);
	cnamelen = strlen (klass->name);
	if (nspacelen + cnamelen + 2 > bufsize)
		return 0;
	if (nspacelen) {
		memcpy (buf, klass->name_space, nspacelen);
		buf [nspacelen ++] = '.';
	}
	memcpy (buf + nspacelen, klass->name, cnamelen);
	buf [nspacelen + cnamelen] = 0;
	return nspacelen + cnamelen;
}

gpointer
mono_lookup_internal_call (MonoMethod *method)
{
	char *sigstart;
	char *tmpsig;
	char mname [2048];
	int typelen = 0, mlen, siglen;
	gpointer res;
	const IcallMap *imap;

	g_assert (method != NULL);

	typelen = concat_class_name (mname, sizeof (mname), method->klass);
	if (!typelen)
		return NULL;

	imap = find_class_icalls (mname);

	mname [typelen] = ':';
	mname [typelen + 1] = ':';

	mlen = strlen (method->name);
	memcpy (mname + typelen + 2, method->name, mlen);
	sigstart = mname + typelen + 2 + mlen;
	*sigstart = 0;

	tmpsig = mono_signature_get_desc (method->signature, TRUE);
	siglen = strlen (tmpsig);
	if (typelen + mlen + siglen + 6 > sizeof (mname))
		return NULL;
	sigstart [0] = '(';
	memcpy (sigstart + 1, tmpsig, siglen);
	sigstart [siglen + 1] = ')';
	sigstart [siglen + 2] = 0;
	g_free (tmpsig);
	
	mono_loader_lock ();

	res = g_hash_table_lookup (icall_hash, mname);
	if (res) {
		mono_loader_unlock ();
		return res;
	}
	/* try without signature */
	*sigstart = 0;
	res = g_hash_table_lookup (icall_hash, mname);
	if (res) {
		mono_loader_unlock ();
		return res;
	}

	/* it wasn't found in the static call tables */
	if (!imap) {
		mono_loader_unlock ();
		return NULL;
	}
	res = find_method_icall (imap, sigstart - mlen);
	if (res) {
		mono_loader_unlock ();
		return res;
	}
	/* try _with_ signature */
	*sigstart = '(';
	res = find_method_icall (imap, sigstart - mlen);
	if (res) {
		mono_loader_unlock ();
		return res;
	}
	
	g_warning ("cant resolve internal call to \"%s\" (tested without signature also)", mname);
	g_print ("\nYour mono runtime and class libraries are out of sync.\n");
	g_print ("The out of sync library is: %s\n", method->klass->image->name);
	g_print ("\nWhen you update one from cvs you need to update, compile and install\nthe other too.\n");
	g_print ("Do not report this as a bug unless you're sure you have updated correctly:\nyou probably have a broken mono install.\n");
	g_print ("If you see other errors or faults after this message they are probably related\n");
	g_print ("and you need to fix your mono install first.\n");

	mono_loader_unlock ();

	return NULL;
}

