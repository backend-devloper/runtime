/*
 * object.c: Object creation for the Mono runtime
 *
 * Author:
 *   Miguel de Icaza (miguel@ximian.com)
 *   Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2001-2004 Ximian, Inc.
 */
#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/object.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/domain-internals.h>
#include "mono/metadata/metadata-internals.h"
#include "mono/metadata/class-internals.h"
#include <mono/metadata/assembly.h>
#include <mono/metadata/threadpool.h>
#include <mono/metadata/marshal.h>
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/marshal.h"
#include <mono/metadata/threads.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/environment.h>
#include "mono/metadata/profiler-private.h"
#include "mono/metadata/security-manager.h"
#include <mono/os/gc_wrapper.h>
#include <mono/utils/strenc.h>

#ifdef HAVE_BOEHM_GC
#define NEED_TO_ZERO_PTRFREE 1
#define ALLOC_PTRFREE(obj,vt,size) do { (obj) = GC_MALLOC_ATOMIC ((size)); (obj)->vtable = (vt); (obj)->synchronisation = NULL;} while (0)
#define ALLOC_OBJECT(obj,vt,size) do { (obj) = GC_MALLOC ((size)); (obj)->vtable = (vt);} while (0)
#ifdef HAVE_GC_GCJ_MALLOC
#define CREATION_SPEEDUP 1
#define GC_NO_DESCRIPTOR ((gpointer)(0 | GC_DS_LENGTH))
#define ALLOC_TYPED(dest,size,type) do { (dest) = GC_GCJ_MALLOC ((size),(type)); } while (0)
#define MAKE_STRING_DESCRIPTOR(bitmap,sz) GC_make_descriptor((GC_bitmap)(bitmap),(sz))
#define MAKE_DESCRIPTOR(bitmap,sz,objsize) GC_make_descriptor((GC_bitmap)(bitmap),(sz))
#else
#define GC_NO_DESCRIPTOR (NULL)
#define ALLOC_TYPED(dest,size,type) do { (dest) = GC_MALLOC ((size)); *(gpointer*)dest = (type);} while (0)
#define MAKE_STRING_DESCRIPTOR(bitmap,sz) NULL
#define MAKE_DESCRIPTOR(bitmap,sz,objsize) NULL
#endif
#else
#ifdef HAVE_SGEN_GC
#define GC_NO_DESCRIPTOR (NULL)
#define ALLOC_PTRFREE(obj,vt,size) do { (obj) = mono_gc_alloc_obj (vt, size);} while (0)
#define ALLOC_OBJECT(obj,vt,size) do { (obj) = mono_gc_alloc_obj (vt, size);} while (0)
#define ALLOC_TYPED(dest,size,type) do { (dest) = mono_gc_alloc_obj (type, size);} while (0)
#define MAKE_STRING_DESCRIPTOR(bitmap,sz) mono_gc_make_descr_for_string ()
#define MAKE_DESCRIPTOR(bitmap,sz,objsize) mono_gc_make_descr_for_object ((bitmap), (sz), (objsize))
#else
#define NEED_TO_ZERO_PTRFREE 1
#define GC_NO_DESCRIPTOR (NULL)
#define ALLOC_PTRFREE(obj,vt,size) do { (obj) = malloc ((size)); (obj)->vtable = (vt); (obj)->synchronisation = NULL;} while (0)
#define ALLOC_OBJECT(obj,vt,size) do { (obj) = calloc (1, (size)); (obj)->vtable = (vt);} while (0)
#define ALLOC_TYPED(dest,size,type) do { (dest) = calloc (1, (size)); *(gpointer*)dest = (type);} while (0)
#define MAKE_STRING_DESCRIPTOR(bitmap,sz) NULL
#define MAKE_DESCRIPTOR(bitmap,sz,objsize) NULL
#endif
#endif

static MonoObject* mono_object_new_ptrfree (MonoVTable *vtable);
static MonoObject* mono_object_new_ptrfree_box (MonoVTable *vtable);

static void
get_default_field_value (MonoDomain* domain, MonoClassField *field, void *value);

static MonoString*
mono_ldstr_metdata_sig (MonoDomain *domain, const char* sig);

void
mono_runtime_object_init (MonoObject *this)
{
	MonoMethod *method = NULL;
	MonoClass *klass = this->vtable->klass;

	method = mono_class_get_method_from_name (klass, ".ctor", 0);
	g_assert (method);

	if (method->klass->valuetype)
		this = mono_object_unbox (this);
	mono_runtime_invoke (method, this, NULL, NULL);
}

/* The pseudo algorithm for type initialization from the spec
Note it doesn't say anything about domains - only threads.

2. If the type is initialized you are done.
2.1. If the type is not yet initialized, try to take an 
     initialization lock.  
2.2. If successful, record this thread as responsible for 
     initializing the type and proceed to step 2.3.
2.2.1. If not, see whether this thread or any thread 
     waiting for this thread to complete already holds the lock.
2.2.2. If so, return since blocking would create a deadlock.  This thread 
     will now see an incompletely initialized state for the type, 
     but no deadlock will arise.
2.2.3  If not, block until the type is initialized then return.
2.3 Initialize the parent type and then all interfaces implemented 
    by this type.
2.4 Execute the type initialization code for this type.
2.5 Mark the type as initialized, release the initialization lock, 
    awaken any threads waiting for this type to be initialized, 
    and return.

*/

typedef struct
{
	guint32 initializing_tid;
	guint32 waiting_count;
	gboolean done;
	CRITICAL_SECTION initialization_section;
} TypeInitializationLock;

/* for locking access to type_initialization_hash and blocked_thread_hash */
static CRITICAL_SECTION type_initialization_section;

/* from vtable to lock */
static GHashTable *type_initialization_hash;

/* from thread id to thread id being waited on */
static GHashTable *blocked_thread_hash;

/* Main thread */
static MonoThread *main_thread;

/**
 * mono_thread_set_main:
 * @thread: thread to set as the main thread
 *
 * This function can be used to instruct the runtime to treat @thread
 * as the main thread, ie, the thread that would normally execute the Main()
 * method. This basically means that at the end of @thread, the runtime will
 * wait for the existing foreground threads to quit and other such details.
 */
void
mono_thread_set_main (MonoThread *thread)
{
	main_thread = thread;
}

MonoThread*
mono_thread_get_main (void)
{
	return main_thread;
}

void
mono_type_initialization_init (void)
{
	InitializeCriticalSection (&type_initialization_section);
	type_initialization_hash = g_hash_table_new (NULL, NULL);
	blocked_thread_hash = g_hash_table_new (NULL, NULL);
}

/*
 * mono_runtime_class_init:
 * @vtable: vtable that needs to be initialized
 *
 * This routine calls the class constructor for @vtable.
 */
void
mono_runtime_class_init (MonoVTable *vtable)
{
	MonoException *exc;
	MonoException *exc_to_throw;
	MonoMethod *method = NULL;
	MonoClass *klass;
	gchar *full_name;

	MONO_ARCH_SAVE_REGS;

	if (vtable->initialized)
		return;

	exc = NULL;
	klass = vtable->klass;

	method = mono_class_get_cctor (klass);

	if (method) {
		MonoDomain *domain = vtable->domain;
		TypeInitializationLock *lock;
		guint32 tid = GetCurrentThreadId();
		int do_initialization = 0;
		MonoDomain *last_domain = NULL;

		EnterCriticalSection (&type_initialization_section);
		/* double check... */
		if (vtable->initialized) {
			LeaveCriticalSection (&type_initialization_section);
			return;
		}
		lock = g_hash_table_lookup (type_initialization_hash, vtable);
		if (lock == NULL) {
			/* This thread will get to do the initialization */
			if (mono_domain_get () != domain) {
				/* Transfer into the target domain */
				last_domain = mono_domain_get ();
				if (!mono_domain_set (domain, FALSE)) {
					vtable->initialized = 1;
					LeaveCriticalSection (&type_initialization_section);
					mono_raise_exception (mono_get_exception_appdomain_unloaded ());
				}
			}
			lock = g_malloc (sizeof(TypeInitializationLock));
			InitializeCriticalSection (&lock->initialization_section);
			lock->initializing_tid = tid;
			lock->waiting_count = 1;
			lock->done = FALSE;
			/* grab the vtable lock while this thread still owns type_initialization_section */
			EnterCriticalSection (&lock->initialization_section);
			g_hash_table_insert (type_initialization_hash, vtable, lock);
			do_initialization = 1;
		} else {
			gpointer blocked;
			TypeInitializationLock *pending_lock;

			if (lock->initializing_tid == tid || lock->done) {
				LeaveCriticalSection (&type_initialization_section);
				return;
			}
			/* see if the thread doing the initialization is already blocked on this thread */
			blocked = GUINT_TO_POINTER (lock->initializing_tid);
			while ((pending_lock = (TypeInitializationLock*) g_hash_table_lookup (blocked_thread_hash, blocked))) {
				if (pending_lock->initializing_tid == tid) {
					if (!pending_lock->done) {
						LeaveCriticalSection (&type_initialization_section);
						return;
					} else {
						/* the thread doing the initialization is blocked on this thread,
						   but on a lock that has already been freed. It just hasn't got
						   time to awake */
						break;
					}
				}
				blocked = GUINT_TO_POINTER (pending_lock->initializing_tid);
			}
			++lock->waiting_count;
			/* record the fact that we are waiting on the initializing thread */
			g_hash_table_insert (blocked_thread_hash, GUINT_TO_POINTER (tid), lock);
		}
		LeaveCriticalSection (&type_initialization_section);

		if (do_initialization) {
			mono_runtime_invoke (method, NULL, NULL, (MonoObject **) &exc);
			if (last_domain)
				mono_domain_set (last_domain, TRUE);
			lock->done = TRUE;
			LeaveCriticalSection (&lock->initialization_section);
		} else {
			/* this just blocks until the initializing thread is done */
			EnterCriticalSection (&lock->initialization_section);
			LeaveCriticalSection (&lock->initialization_section);
		}

		EnterCriticalSection (&type_initialization_section);
		if (lock->initializing_tid != tid)
			g_hash_table_remove (blocked_thread_hash, GUINT_TO_POINTER (tid));
		--lock->waiting_count;
		if (lock->waiting_count == 0) {
			DeleteCriticalSection (&lock->initialization_section);
			g_hash_table_remove (type_initialization_hash, vtable);
			g_free (lock);
		}
		vtable->initialized = 1;
		/* FIXME: if the cctor fails, the type must be marked as unusable */
		LeaveCriticalSection (&type_initialization_section);
	} else {
		vtable->initialized = 1;
		return;
	}

	if (exc == NULL ||
	    (klass->image == mono_defaults.corlib &&		
	     !strcmp (klass->name_space, "System") &&
	     !strcmp (klass->name, "TypeInitializationException")))
		return; /* No static constructor found or avoid infinite loop */

	if (klass->name_space && *klass->name_space)
		full_name = g_strdup_printf ("%s.%s", klass->name_space, klass->name);
	else
		full_name = g_strdup (klass->name);

	exc_to_throw = mono_get_exception_type_initialization (full_name, exc);
	g_free (full_name);

	mono_raise_exception (exc_to_throw);
}

static
gboolean release_type_locks (gpointer key, gpointer value, gpointer user)
{
	TypeInitializationLock *lock = (TypeInitializationLock*) value;
	if (lock->initializing_tid == GPOINTER_TO_UINT (user) && !lock->done) {
		lock->done = TRUE;
		LeaveCriticalSection (&lock->initialization_section);
		--lock->waiting_count;
		if (lock->waiting_count == 0) {
			DeleteCriticalSection (&lock->initialization_section);
			g_free (lock);
			return TRUE;
		}
	}
	return FALSE;
}

void
mono_release_type_locks (MonoThread *thread)
{
	EnterCriticalSection (&type_initialization_section);
	g_hash_table_foreach_remove (type_initialization_hash, release_type_locks, GUINT_TO_POINTER (thread->tid));
	LeaveCriticalSection (&type_initialization_section);
}

static gpointer
default_trampoline (MonoMethod *method)
{
	return method;
}

static gpointer
default_remoting_trampoline (MonoMethod *method, MonoRemotingTarget target)
{
	g_error ("remoting not installed");
	return NULL;
}

static MonoTrampoline arch_create_jit_trampoline = default_trampoline;
static MonoRemotingTrampoline arch_create_remoting_trampoline = default_remoting_trampoline;

void
mono_install_trampoline (MonoTrampoline func) 
{
	arch_create_jit_trampoline = func? func: default_trampoline;
}

void
mono_install_remoting_trampoline (MonoRemotingTrampoline func) 
{
	arch_create_remoting_trampoline = func? func: default_remoting_trampoline;
}

static MonoCompileFunc default_mono_compile_method = NULL;

/**
 * mono_install_compile_method:
 * @func: function to install
 *
 * This is a VM internal routine
 */
void        
mono_install_compile_method (MonoCompileFunc func)
{
	default_mono_compile_method = func;
}

/**
 * mono_compile_method:
 * @method: The method to compile.
 *
 * This JIT-compiles the method, and returns the pointer to the native code
 * produced.
 */
gpointer 
mono_compile_method (MonoMethod *method)
{
	if (!default_mono_compile_method) {
		g_error ("compile method called on uninitialized runtime");
		return NULL;
	}
	return default_mono_compile_method (method);
}

static MonoFreeMethodFunc default_mono_free_method = NULL;

/**
 * mono_install_free_method:
 * @func: pointer to the MonoFreeMethodFunc used to release a method
 *
 * This is an internal VM routine, it is used for the engines to
 * register a handler to release the resources associated with a method.
 *
 * Methods are freed when no more references to the delegate that holds
 * them are left.
 */
void
mono_install_free_method (MonoFreeMethodFunc func)
{
	default_mono_free_method = func;
}

/**
 * mono_runtime_free_method:
 * @domain; domain where the method is hosted
 * @method: method to release
 *
 * This routine is invoked to free the resources associated with
 * a method that has been JIT compiled.  This is used to discard
 * methods that were used only temporarily (for example, used in marshalling)
 *
 */
void
mono_runtime_free_method (MonoDomain *domain, MonoMethod *method)
{
	if (default_mono_free_method != NULL)
		default_mono_free_method (domain, method);

	mono_free_method (method);
}

static MonoInitVTableFunc init_vtable_func = NULL;

/**
 * mono_install_init_vtable:
 * @func: pointer to the function to be installed
 *
 *   Register a function which will be called by the runtime to initialize the
 * method pointers inside a vtable. The JIT can use this function to load the
 * vtable from the AOT file for example.
 */
void
mono_install_init_vtable (MonoInitVTableFunc func)
{
	init_vtable_func = func;
}

/*
 * The vtables in the root appdomain are assumed to be reachable by other 
 * roots, and we don't use typed allocation in the other domains.
 */

/* The sync block is no longer a GC pointer */
#define GC_HEADER_BITMAP (0)

#define BITMAP_EL_SIZE (sizeof (gsize) * 8)

static gsize*
compute_class_bitmap (MonoClass *class, gsize *bitmap, int size, int offset, int *max_set)
{
	MonoClassField *field;
	MonoClass *p;
	guint32 pos;
	int max_size = class->instance_size / sizeof (gpointer);
	if (max_size > size) {
		bitmap = g_malloc0 (sizeof (gsize) * ((max_size) + 1));
	}

	for (p = class; p != NULL; p = p->parent) {
		gpointer iter = NULL;
		while ((field = mono_class_get_fields (p, &iter))) {
			MonoType *type;

			if (field->type->attrs & (FIELD_ATTRIBUTE_STATIC | FIELD_ATTRIBUTE_HAS_FIELD_RVA))
				continue;
			/* FIXME: should not happen, flag as type load error */
			if (field->type->byref)
				break;

			pos = field->offset / sizeof (gpointer);
			pos += offset;

			type = mono_type_get_underlying_type (field->type);
			switch (type->type) {
			/* FIXME: _I and _U and _PTR should be removed eventually */
			case MONO_TYPE_I:
			case MONO_TYPE_U:
			case MONO_TYPE_PTR:
			case MONO_TYPE_FNPTR:
			case MONO_TYPE_STRING:
			case MONO_TYPE_SZARRAY:
			case MONO_TYPE_CLASS:
			case MONO_TYPE_OBJECT:
			case MONO_TYPE_ARRAY:
				g_assert ((field->offset % sizeof(gpointer)) == 0);

				bitmap [pos / BITMAP_EL_SIZE] |= ((gsize)1) << (pos % BITMAP_EL_SIZE);
				*max_set = MAX (*max_set, pos);
				break;
			case MONO_TYPE_VALUETYPE: {
				MonoClass *fclass = field->type->data.klass;
				if (fclass->has_references) {
					/* remove the object header */
					compute_class_bitmap (fclass, bitmap, size, pos - (sizeof (MonoObject) / sizeof (gpointer)), max_set);
				}
				break;
			}
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
			case MONO_TYPE_BOOLEAN:
			case MONO_TYPE_CHAR:
				break;
			default:
				g_assert_not_reached ();
				break;
			}
		}
	}
	return bitmap;
}

static void
mono_class_compute_gc_descriptor (MonoClass *class)
{
	int max_set = 0;
	gsize *bitmap;
	gsize default_bitmap [4] = {0};
	static gboolean gcj_inited = FALSE;

	if (!gcj_inited) {
		mono_loader_lock ();

		mono_register_jit_icall (mono_object_new_ptrfree, "mono_object_new_ptrfree", mono_create_icall_signature ("object ptr"), FALSE);
		mono_register_jit_icall (mono_object_new_ptrfree_box, "mono_object_new_ptrfree_box", mono_create_icall_signature ("object ptr"), FALSE);
		mono_register_jit_icall (mono_object_new_fast, "mono_object_new_fast", mono_create_icall_signature ("object ptr"), FALSE);

#ifdef HAVE_GC_GCJ_MALLOC

		GC_init_gcj_malloc (5, NULL);

#ifdef GC_REDIRECT_TO_LOCAL
		mono_register_jit_icall (GC_local_gcj_malloc, "GC_local_gcj_malloc", mono_create_icall_signature ("object int ptr"), FALSE);
		mono_register_jit_icall (GC_local_gcj_fast_malloc, "GC_local_gcj_fast_malloc", mono_create_icall_signature ("object int ptr"), FALSE);
#endif
		mono_register_jit_icall (GC_gcj_malloc, "GC_gcj_malloc", mono_create_icall_signature ("object int ptr"), FALSE);
		mono_register_jit_icall (GC_gcj_fast_malloc, "GC_gcj_fast_malloc", mono_create_icall_signature ("object int ptr"), FALSE);
#endif
		gcj_inited = TRUE;
		mono_loader_unlock ();
	}

	if (!class->inited)
		mono_class_init (class);

	if (class->gc_descr_inited)
		return;

	class->gc_descr_inited = TRUE;
	class->gc_descr = GC_NO_DESCRIPTOR;

	bitmap = default_bitmap;
	if (class == mono_defaults.string_class) {
		class->gc_descr = (gpointer)MAKE_STRING_DESCRIPTOR (bitmap, 2);
	} else if (class->rank) {
		mono_class_compute_gc_descriptor (class->element_class);
#ifdef HAVE_SGEN_GC
		/* libgc has no usable support for arrays... */
		if (!class->element_class->valuetype) {
			gsize abm = 1;
			class->gc_descr = mono_gc_make_descr_for_array (TRUE, &abm, 1, sizeof (gpointer));
			/*printf ("new array descriptor: 0x%x for %s.%s\n", class->gc_descr,
				class->name_space, class->name);*/
		} else {
			/* remove the object header */
			bitmap = compute_class_bitmap (class->element_class, default_bitmap, sizeof (default_bitmap) * 8, - (sizeof (MonoObject) / sizeof (gpointer)), &max_set);
			class->gc_descr = mono_gc_make_descr_for_array (TRUE, bitmap, mono_array_element_size (class) / sizeof (gpointer), mono_array_element_size (class));
			/*printf ("new vt array descriptor: 0x%x for %s.%s\n", class->gc_descr,
				class->name_space, class->name);*/
			if (bitmap != default_bitmap)
				g_free (bitmap);
		}
#endif
	} else {
		/*static int count = 0;
		if (count++ > 58)
			return;*/
		bitmap = compute_class_bitmap (class, default_bitmap, sizeof (default_bitmap) * 8, 0, &max_set);
#ifdef HAVE_BOEHM_GC
		/* It seems there are issues when the bitmap doesn't fit: play it safe */
		if (max_set >= 30) {
			/*g_print ("disabling typed alloc (%d) for %s.%s\n", max_set, class->name_space, class->name);*/
			if (bitmap != default_bitmap)
				g_free (bitmap);
			return;
		}
#endif
		class->gc_descr = (gpointer)MAKE_DESCRIPTOR (bitmap, max_set + 1, class->instance_size);
		/*printf ("new descriptor: %p 0x%x for %s.%s\n", class->gc_descr, bitmap [0], class->name_space, class->name);*/
		if (bitmap != default_bitmap)
			g_free (bitmap);
	}
}

/**
 * field_is_special_static:
 * @fklass: The MonoClass to look up.
 * @field: The MonoClassField describing the field.
 *
 * Returns: SPECIAL_STATIC_THREAD if the field is thread static, SPECIAL_STATIC_CONTEXT if it is context static,
 * SPECIAL_STATIC_NONE otherwise.
 */
static gint32
field_is_special_static (MonoClass *fklass, MonoClassField *field)
{
	MonoCustomAttrInfo *ainfo;
	int i;
	ainfo = mono_custom_attrs_from_field (fklass, field);
	if (!ainfo)
		return FALSE;
	for (i = 0; i < ainfo->num_attrs; ++i) {
		MonoClass *klass = ainfo->attrs [i].ctor->klass;
		if (klass->image == mono_defaults.corlib) {
			if (strcmp (klass->name, "ThreadStaticAttribute") == 0) {
				mono_custom_attrs_free (ainfo);
				return SPECIAL_STATIC_THREAD;
			}
			else if (strcmp (klass->name, "ContextStaticAttribute") == 0) {
				mono_custom_attrs_free (ainfo);
				return SPECIAL_STATIC_CONTEXT;
			}
		}
	}
	mono_custom_attrs_free (ainfo);
	return SPECIAL_STATIC_NONE;
}

static MonoVTable *mono_class_create_runtime_vtable (MonoDomain *domain, MonoClass *class);

/**
 * mono_class_vtable:
 * @domain: the application domain
 * @class: the class to initialize
 *
 * VTables are domain specific because we create domain specific code, and 
 * they contain the domain specific static class data.
 */
MonoVTable *
mono_class_vtable (MonoDomain *domain, MonoClass *class)
{
	MonoClassRuntimeInfo *runtime_info;

	g_assert (class);

	/* this check can be inlined in jitted code, too */
	runtime_info = class->runtime_info;
	if (runtime_info && runtime_info->max_domain >= domain->domain_id && runtime_info->domain_vtables [domain->domain_id])
		return runtime_info->domain_vtables [domain->domain_id];
	return mono_class_create_runtime_vtable (domain, class);
}

static MonoVTable *
mono_class_create_runtime_vtable (MonoDomain *domain, MonoClass *class)
{
	MonoVTable *vt;
	MonoClassRuntimeInfo *runtime_info, *old_info;
	MonoClassField *field;
	char *t;
	int i;
	gboolean inited = FALSE;
	guint32 vtable_size;
	guint32 cindex;
	guint32 constant_cols [MONO_CONSTANT_SIZE];
	gpointer iter;

	mono_domain_lock (domain);
	runtime_info = class->runtime_info;
	if (runtime_info && runtime_info->max_domain >= domain->domain_id && runtime_info->domain_vtables [domain->domain_id]) {
		mono_domain_unlock (domain);
		return runtime_info->domain_vtables [domain->domain_id];
	}
	if (!class->inited)
		mono_class_init (class);

	mono_stats.used_class_count++;
	mono_stats.class_vtable_size += sizeof (MonoVTable) + class->vtable_size * sizeof (gpointer);

	vtable_size = sizeof (MonoVTable) + class->vtable_size * sizeof (gpointer);

	vt = mono_mempool_alloc0 (domain->mp,  vtable_size);

	vt->klass = class;
	vt->rank = class->rank;
	vt->domain = domain;

	mono_class_compute_gc_descriptor (class);
		/*
		 * We can't use typed allocation in the non-root domains, since the
		 * collector needs the GC descriptor stored in the vtable even after
		 * the mempool containing the vtable is destroyed when the domain is
		 * unloaded. An alternative might be to allocate vtables in the GC
		 * heap, but this does not seem to work (it leads to crashes inside
		 * libgc). If that approach is tried, two gc descriptors need to be
		 * allocated for each class: one for the root domain, and one for all
		 * other domains. The second descriptor should contain a bit for the
		 * vtable field in MonoObject, since we can no longer assume the 
		 * vtable is reachable by other roots after the appdomain is unloaded.
		 */
#ifdef HAVE_BOEHM_GC
	if (domain != mono_get_root_domain ())
		vt->gc_descr = GC_NO_DESCRIPTOR;
	else
#endif
		vt->gc_descr = class->gc_descr;

	if (class->class_size) {
		if (class->has_static_refs)
			vt->data = mono_gc_alloc_fixed (class->class_size, NULL);
		else
			vt->data = mono_mempool_alloc0 (domain->mp, class->class_size);
		mono_g_hash_table_insert (domain->static_data_hash, class, vt->data);
		mono_stats.class_static_data_size += class->class_size;
	}

	cindex = -1;
	iter = NULL;
	while ((field = mono_class_get_fields (class, &iter))) {
		if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC))
			continue;
		if (mono_field_is_deleted (field))
			continue;
		if (!(field->type->attrs & FIELD_ATTRIBUTE_LITERAL)) {
			gint32 special_static = field_is_special_static (class, field);
			if (special_static != SPECIAL_STATIC_NONE) {
				guint32 size, align, offset;
				size = mono_type_size (field->type, &align);
				offset = mono_alloc_special_static_data (special_static, size, align);
				if (!domain->special_static_fields)
					domain->special_static_fields = g_hash_table_new (NULL, NULL);
				g_hash_table_insert (domain->special_static_fields, field, GUINT_TO_POINTER (offset));
				continue;
			}
		}
		if ((field->type->attrs & FIELD_ATTRIBUTE_HAS_FIELD_RVA)) {
			MonoClass *fklass = mono_class_from_mono_type (field->type);
			g_assert (!(field->type->attrs & FIELD_ATTRIBUTE_HAS_DEFAULT));
			t = (char*)vt->data + field->offset;
			if (fklass->valuetype) {
				memcpy (t, field->data, mono_class_value_size (fklass, NULL));
			} else {
				/* it's a pointer type: add check */
				g_assert (fklass->byval_arg.type == MONO_TYPE_PTR);
				*t = *(char *)field->data;
			}
			continue;
		}
		if (!(field->type->attrs & FIELD_ATTRIBUTE_HAS_DEFAULT))
			continue;

		/* later do this only on demand if needed */
		if (!field->data) {
			cindex = mono_metadata_get_constant_index (class->image, mono_class_get_field_token (field), cindex + 1);
			g_assert (cindex);
			g_assert (!(field->type->attrs & FIELD_ATTRIBUTE_HAS_FIELD_RVA));

			mono_metadata_decode_row (&class->image->tables [MONO_TABLE_CONSTANT], cindex - 1, constant_cols, MONO_CONSTANT_SIZE);
			field->def_type = constant_cols [MONO_CONSTANT_TYPE];
			field->data = (gpointer)mono_metadata_blob_heap (class->image, constant_cols [MONO_CONSTANT_VALUE]);
		}
		
	}

	vt->max_interface_id = class->max_interface_id;
	
	vt->interface_offsets = mono_mempool_alloc0 (domain->mp, 
	        sizeof (gpointer) * (class->max_interface_id + 1));

	/* initialize interface offsets */
	for (i = 0; i <= class->max_interface_id; ++i) {
		int slot = class->interface_offsets [i];
		if (slot >= 0)
			vt->interface_offsets [i] = &(vt->vtable [slot]);
	}

	/* 
	 * arch_create_jit_trampoline () can recursively call this function again
	 * because it compiles icall methods right away.
	 */
	/* FIXME: class_vtable_hash is basically obsolete now: remove as soon
	 * as we change the code in appdomain.c to invalidate vtables by
	 * looking at the possible MonoClasses created for the domain.
	 * Or we can reuse static_data_hash, by using vtable as a key
	 * and always inserting into that hash.
	 */
	g_hash_table_insert (domain->class_vtable_hash, class, vt);
	/* class->runtime_info is protected by the loader lock, both when
	 * it it enlarged and when it is stored info.
	 */
	mono_loader_lock ();
	old_info = class->runtime_info;
	if (old_info && old_info->max_domain >= domain->domain_id) {
		/* someone already created a large enough runtime info */
		old_info->domain_vtables [domain->domain_id] = vt;
	} else {
		int new_size = domain->domain_id;
		if (old_info)
			new_size = MAX (new_size, old_info->max_domain);
		new_size++;
		/* make the new size a power of two */
		i = 2;
		while (new_size > i)
			i <<= 1;
		new_size = i;
		/* this is a bounded memory retention issue: may want to 
		 * handle it differently when we'll have a rcu-like system.
		 */
		runtime_info = mono_mempool_alloc0 (class->image->mempool, sizeof (MonoClassRuntimeInfo) + new_size * sizeof (gpointer));
		runtime_info->max_domain = new_size - 1;
		/* copy the stuff from the older info */
		if (old_info) {
			memcpy (runtime_info->domain_vtables, old_info->domain_vtables, (old_info->max_domain + 1) * sizeof (gpointer));
		}
		runtime_info->domain_vtables [domain->domain_id] = vt;
		/* keep this last (add membarrier) */
		class->runtime_info = runtime_info;
	}
	mono_loader_unlock ();

	/* initialize vtable */
	if (init_vtable_func)
		inited = init_vtable_func (vt);

	if (!inited) {
		mono_class_setup_vtable (class);

		for (i = 0; i < class->vtable_size; ++i) {
			MonoMethod *cm;

			if ((cm = class->vtable [i])) {
				if (mono_method_signature (cm)->generic_param_count)
					vt->vtable [i] = cm;
				else
					vt->vtable [i] = arch_create_jit_trampoline (cm);
			}
		}
	}

	mono_domain_unlock (domain);

	/* Initialization is now complete, we can throw if the InheritanceDemand aren't satisfied */
	if (mono_is_security_manager_active () && (class->exception_type == MONO_EXCEPTION_SECURITY_INHERITANCEDEMAND)) {
		MonoException *exc = mono_class_get_exception_for_failure (class);
		g_assert (exc);
		mono_raise_exception (exc);
	}

	/* make sure the the parent is initialized */
	if (class->parent)
		mono_class_vtable (domain, class->parent);

	vt->type = mono_type_get_object (domain, &class->byval_arg);
	if (class->contextbound)
		vt->remote = 1;
	else
		vt->remote = 0;

	return vt;
}

/**
 * mono_class_proxy_vtable:
 * @domain: the application domain
 * @remove_class: the remote class
 *
 * Creates a vtable for transparent proxies. It is basically
 * a copy of the real vtable of the class wrapped in @remote_class,
 * but all function pointers invoke the remoting functions, and
 * vtable->klass points to the transparent proxy class, and not to @class.
 */
static MonoVTable *
mono_class_proxy_vtable (MonoDomain *domain, MonoRemoteClass *remote_class, MonoRemotingTarget target_type)
{
	MonoVTable *vt, *pvt;
	int i, j, vtsize, max_interface_id, extra_interface_vtsize = 0;
	MonoClass *k;
	MonoClass *class = remote_class->proxy_class;

	vt = mono_class_vtable (domain, class);
	max_interface_id = vt->max_interface_id;

	/* Calculate vtable space for extra interfaces */
	for (j = 0; j < remote_class->interface_count; j++) {
		MonoClass* iclass = remote_class->interfaces[j];
		int method_count = mono_class_num_methods (iclass);
	
		if (iclass->interface_id <= class->max_interface_id && class->interface_offsets[iclass->interface_id] != 0) 
			continue;	/* interface implemented by the class */

		for (i = 0; i < iclass->interface_count; i++)
			method_count += mono_class_num_methods (iclass->interfaces[i]);

		extra_interface_vtsize += method_count * sizeof (gpointer);
		if (iclass->max_interface_id > max_interface_id) max_interface_id = iclass->max_interface_id;
	}

	vtsize = sizeof (MonoVTable) + class->vtable_size * sizeof (gpointer);

	mono_stats.class_vtable_size += vtsize + extra_interface_vtsize;

	pvt = mono_mempool_alloc (domain->mp, vtsize + extra_interface_vtsize);
	memcpy (pvt, vt, vtsize);

	pvt->klass = mono_defaults.transparent_proxy_class;
	/* we need to keep the GC descriptor for a transparent proxy or we confuse the precise GC */
	pvt->gc_descr = mono_defaults.transparent_proxy_class->gc_descr;

	/* initialize vtable */
	mono_class_setup_vtable (class);
	for (i = 0; i < class->vtable_size; ++i) {
		MonoMethod *cm;
		    
		if ((cm = class->vtable [i]))
			pvt->vtable [i] = arch_create_remoting_trampoline (cm, target_type);
	}

	if (class->flags & TYPE_ATTRIBUTE_ABSTRACT) {
		/* create trampolines for abstract methods */
		for (k = class; k; k = k->parent) {
			MonoMethod* m;
			gpointer iter = NULL;
			while ((m = mono_class_get_methods (k, &iter)))
				if (!pvt->vtable [m->slot])
					pvt->vtable [m->slot] = arch_create_remoting_trampoline (m, target_type);
		}
	}

	pvt->max_interface_id = max_interface_id;
	pvt->interface_offsets = mono_mempool_alloc0 (domain->mp, 
			sizeof (gpointer) * (max_interface_id + 1));

	/* initialize interface offsets */
	for (i = 0; i <= class->max_interface_id; ++i) {
		int slot = class->interface_offsets [i];
		if (slot >= 0)
			pvt->interface_offsets [i] = &(pvt->vtable [slot]);
	}

	if (remote_class->interface_count > 0)
	{
		int slot = class->vtable_size;
		MonoClass* interf;
		MonoClass* iclass;
		int n;

		/* Create trampolines for the methods of the interfaces */
		for (n = 0; n < remote_class->interface_count; n++) 
		{
			iclass = remote_class->interfaces[n];
			if (iclass->interface_id <= class->max_interface_id && class->interface_offsets[iclass->interface_id] != 0) 
				continue;	/* interface implemented by the class */
		
			i = -1;
			interf = iclass;
			do {
				MonoMethod* cm;
				gpointer iter;
				
				pvt->interface_offsets [interf->interface_id] = &pvt->vtable [slot];
	
				iter = NULL;
				j = 0;
				while ((cm = mono_class_get_methods (interf, &iter)))
					pvt->vtable [slot + j++] = arch_create_remoting_trampoline (cm, target_type);
				
				slot += mono_class_num_methods (interf);
				if (++i < iclass->interface_count) interf = iclass->interfaces[i];
				else interf = NULL;
				
			} while (interf);
		}
	}

	return pvt;
}

/**
 * mono_remote_class:
 * @domain: the application domain
 * @class_name: name of the remote class
 *
 * Creates and initializes a MonoRemoteClass object for a remote type. 
 * 
 */
MonoRemoteClass*
mono_remote_class (MonoDomain *domain, MonoString *class_name, MonoClass *proxy_class)
{
	MonoRemoteClass *rc;

	mono_domain_lock (domain);
	rc = mono_g_hash_table_lookup (domain->proxy_vtable_hash, class_name);

	if (rc) {
		mono_domain_unlock (domain);
		return rc;
	}

	rc = mono_mempool_alloc (domain->mp, sizeof(MonoRemoteClass));
	rc->default_vtable = NULL;
	rc->xdomain_vtable = NULL;
	rc->interface_count = 0;
	rc->interfaces = NULL;
	rc->proxy_class = mono_defaults.marshalbyrefobject_class;
	rc->proxy_class_name = mono_string_to_utf8 (class_name);

	mono_g_hash_table_insert (domain->proxy_vtable_hash, class_name, rc);
	mono_upgrade_remote_class (domain, rc, proxy_class);

	mono_domain_unlock (domain);

	return rc;
}

static void
extend_interface_array (MonoDomain *domain, MonoRemoteClass *remote_class, int amount)
{
	/* Extends the array of interfaces. Memory is extended using blocks of 5 pointers */

	int current_size = ((remote_class->interface_count / 5) + 1) * 5;
	remote_class->interface_count += amount;

	if (remote_class->interface_count > current_size || remote_class->interfaces == NULL) 
	{
		int new_size = ((remote_class->interface_count / 5) + 1) * 5;
		MonoClass **new_array = mono_mempool_alloc (domain->mp, new_size * sizeof (MonoClass*));
	
		if (remote_class->interfaces != NULL)
			memcpy (new_array, remote_class->interfaces, current_size * sizeof (MonoClass*));
		
		remote_class->interfaces = new_array;
	}
}

gpointer
mono_remote_class_vtable (MonoDomain *domain, MonoRemoteClass *remote_class, MonoRealProxy *rp)
{
	if (rp->target_domain_id != -1) {
		if (remote_class->xdomain_vtable == NULL)
			remote_class->xdomain_vtable = mono_class_proxy_vtable (domain, remote_class, MONO_REMOTING_TARGET_APPDOMAIN);
		return remote_class->xdomain_vtable;
	}
	if (remote_class->default_vtable == NULL)
		remote_class->default_vtable = mono_class_proxy_vtable (domain, remote_class, MONO_REMOTING_TARGET_UNKNOWN);
	
	return remote_class->default_vtable;
}


/**
 * mono_upgrade_remote_class:
 * @domain: the application domain
 * @remote_class: the remote class
 * @klass: class to which the remote class can be casted.
 *
 * Updates the vtable of the remote class by adding the necessary method slots
 * and interface offsets so it can be safely casted to klass. klass can be a
 * class or an interface.
 */
void mono_upgrade_remote_class (MonoDomain *domain, MonoRemoteClass *remote_class, MonoClass *klass)
{
	gboolean redo_vtable;

	mono_domain_lock (domain);

	if (klass->flags & TYPE_ATTRIBUTE_INTERFACE) {
		int i;
		redo_vtable = TRUE;
		for (i = 0; i < remote_class->interface_count; i++)
			if (remote_class->interfaces[i] == klass) redo_vtable = FALSE;
				
		if (redo_vtable) {
			extend_interface_array (domain, remote_class, 1);
			remote_class->interfaces [remote_class->interface_count-1] = klass;
		}
	}
	else {
		redo_vtable = (remote_class->proxy_class != klass);
		remote_class->proxy_class = klass;
	}

	if (redo_vtable) {
		remote_class->default_vtable = NULL;
		remote_class->xdomain_vtable = NULL;
	}
/*
	int n;
	printf ("remote class upgrade - class:%s num-interfaces:%d\n", remote_class->proxy_class_name, remote_class->interface_count);
	
	for (n=0; n<remote_class->interface_count; n++)
		printf ("  I:%s\n", remote_class->interfaces[n]->name);
*/

	mono_domain_unlock (domain);
}

/**
 * mono_object_get_virtual_method:
 * @obj: object to operate on.
 * @method: method 
 *
 * Retrieves the MonoMethod that would be called on obj if obj is passed as
 * the instance of a callvirt of method.
 */
MonoMethod*
mono_object_get_virtual_method (MonoObject *obj, MonoMethod *method)
{
	MonoClass *klass;
	MonoMethod **vtable;
	gboolean is_proxy;
	MonoMethod *res = NULL;

	klass = mono_object_class (obj);
	if (klass == mono_defaults.transparent_proxy_class) {
		klass = ((MonoTransparentProxy *)obj)->remote_class->proxy_class;
		is_proxy = TRUE;
	} else {
		is_proxy = FALSE;
	}

	if (!is_proxy && ((method->flags & METHOD_ATTRIBUTE_FINAL) || !(method->flags & METHOD_ATTRIBUTE_VIRTUAL)))
			return method;

	mono_class_setup_vtable (klass);
	vtable = klass->vtable;

	/* check method->slot is a valid index: perform isinstance? */
	if (method->klass->flags & TYPE_ATTRIBUTE_INTERFACE) {
		if (!is_proxy)
			res = vtable [klass->interface_offsets [method->klass->interface_id] + method->slot];
	} else {
		if (method->slot != -1)
			res = vtable [method->slot];
	}

	if (is_proxy) {
		if (!res) res = method;   /* It may be an interface or abstract class method */
		res = mono_marshal_get_remoting_invoke (res);
	}

	g_assert (res);
	
	return res;
}

static MonoObject*
dummy_mono_runtime_invoke (MonoMethod *method, void *obj, void **params, MonoObject **exc)
{
	g_error ("runtime invoke called on uninitialized runtime");
	return NULL;
}

static MonoInvokeFunc default_mono_runtime_invoke = dummy_mono_runtime_invoke;

/**
 * mono_runtime_invoke:
 * @method: method to invoke
 * @obJ: object instance
 * @params: arguments to the method
 * @exc: exception information.
 *
 * Invokes the method represented by @method on the object @obj.
 *
 * obj is the 'this' pointer, it should be NULL for static
 * methods, a MonoObject* for object instances and a pointer to
 * the value type for value types.
 *
 * The params array contains the arguments to the method with the
 * same convention: MonoObject* pointers for object instances and
 * pointers to the value type otherwise. 
 * 
 * From unmanaged code you'll usually use the
 * mono_runtime_invoke() variant.
 *
 * Note that this function doesn't handle virtual methods for
 * you, it will exec the exact method you pass: we still need to
 * expose a function to lookup the derived class implementation
 * of a virtual method (there are examples of this in the code,
 * though).
 * 
 * You can pass NULL as the exc argument if you don't want to
 * catch exceptions, otherwise, *exc will be set to the exception
 * thrown, if any.  if an exception is thrown, you can't use the
 * MonoObject* result from the function.
 * 
 * If the method returns a value type, it is boxed in an object
 * reference.
 */
MonoObject*
mono_runtime_invoke (MonoMethod *method, void *obj, void **params, MonoObject **exc)
{
	return default_mono_runtime_invoke (method, obj, params, exc);
}

static void
set_value (MonoType *type, void *dest, void *value, int deref_pointer)
{
	int t;
	if (type->byref) {
		gpointer *p = (gpointer*)dest;
		*p = value;
		return;
	}
	t = type->type;
handle_enum:
	switch (t) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1: {
		guint8 *p = (guint8*)dest;
		*p = value ? *(guint8*)value : 0;
		return;
	}
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR: {
		guint16 *p = (guint16*)dest;
		*p = value ? *(guint16*)value : 0;
		return;
	}
#if SIZEOF_VOID_P == 4
	case MONO_TYPE_I:
	case MONO_TYPE_U:
#endif
	case MONO_TYPE_I4:
	case MONO_TYPE_U4: {
		gint32 *p = (gint32*)dest;
		*p = value ? *(gint32*)value : 0;
		return;
	}
#if SIZEOF_VOID_P == 8
	case MONO_TYPE_I:
	case MONO_TYPE_U:
#endif
	case MONO_TYPE_I8:
	case MONO_TYPE_U8: {
		gint64 *p = (gint64*)dest;
		*p = value ? *(gint64*)value : 0;
		return;
	}
	case MONO_TYPE_R4: {
		float *p = (float*)dest;
		*p = value ? *(float*)value : 0;
		return;
	}
	case MONO_TYPE_R8: {
		double *p = (double*)dest;
		*p = value ? *(double*)value : 0;
		return;
	}
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_PTR: {
		gpointer *p = (gpointer*)dest;
		*p = deref_pointer? *(gpointer*)value: value;
		return;
	}
	case MONO_TYPE_VALUETYPE:
		if (type->data.klass->enumtype) {
			t = type->data.klass->enum_basetype->type;
			goto handle_enum;
		} else {
			int size;
			size = mono_class_value_size (type->data.klass, NULL);
			if (value == NULL)
				memset (dest, 0, size);
			else
				memcpy (dest, value, size);
		}
		return;
	default:
		g_warning ("got type %x", type->type);
		g_assert_not_reached ();
	}
}

/**
 * mono_field_set_value:
 * @obj: Instance object
 * @field: MonoClassField describing the field to set
 * @value: The value to be set
 *
 * Sets the value of the field described by @field in the object instance @obj
 * to the value passed in @value.
 *
 * The value must be on the native format of the field type. 
 */
void
mono_field_set_value (MonoObject *obj, MonoClassField *field, void *value)
{
	void *dest;

	g_return_if_fail (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC));

	dest = (char*)obj + field->offset;
	set_value (field->type, dest, value, FALSE);
}

/**
 * mono_field_static_set_value:
 * @field: MonoClassField describing the field to set
 * @value: The value to be set
 *
 * Sets the value of the static field described by @field
 * to the value passed in @value.
 *
 * The value must be on the native format of the field type. 
 */
void
mono_field_static_set_value (MonoVTable *vt, MonoClassField *field, void *value)
{
	void *dest;

	g_return_if_fail (field->type->attrs & FIELD_ATTRIBUTE_STATIC);
	/* you cant set a constant! */
	g_return_if_fail (!(field->type->attrs & FIELD_ATTRIBUTE_LITERAL));
	
	dest = (char*)vt->data + field->offset;
	set_value (field->type, dest, value, FALSE);
}

/**
 * mono_field_get_value:
 * @obj: Object instance
 * @field: MonoClassField describing the field to fetch information from
 * @value: pointer to the location where the value will be stored
 *
 * Use this routine to get the value of the field @field in the object
 * passed.
 *
 * The pointer provided by value must be of the field type, for reference
 * types this is a MonoObject*, for value types its the actual pointer to
 * the value type.
 *
 * For example:
 *     int i;
 *     mono_field_get_value (obj, int_field, &i);
 */
void
mono_field_get_value (MonoObject *obj, MonoClassField *field, void *value)
{
	void *src;

	g_return_if_fail (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC));

	src = (char*)obj + field->offset;
	set_value (field->type, value, src, TRUE);
}

/**
 * mono_field_get_value_object:
 * @domain: domain where the object will be created (if boxing)
 * @field: MonoClassField describing the field to fetch information from
 * @obj: The object instance for the field.
 *
 * Returns: a new MonoObject with the value from the given field.  If the
 * field represents a value type, the value is boxed.
 *
 */
MonoObject *
mono_field_get_value_object (MonoDomain *domain, MonoClassField *field, MonoObject *obj)
{	
	MonoObject *o;
	MonoClass *klass;
	MonoVTable *vtable = NULL;
	gchar *v;
	gboolean is_static = FALSE;
	gboolean is_ref = FALSE;

	switch (field->type->type) {
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
		is_ref = field->type->byref;
		break;
	default:
		g_error ("type 0x%x not handled in "
			 "mono_field_get_value_object", field->type->type);
		return NULL;
	}

	if (field->type->attrs & FIELD_ATTRIBUTE_STATIC) {
		is_static = TRUE;
		vtable = mono_class_vtable (domain, field->parent);
		if (!vtable->initialized)
			mono_runtime_class_init (vtable);
	}
	
	if (is_ref) {
		if (is_static) {
			mono_field_static_get_value (vtable, field, &o);
		} else {
			mono_field_get_value (obj, field, &o);
		}
		return o;
	}

	/* boxed value type */
	klass = mono_class_from_mono_type (field->type);
	o = mono_object_new (domain, klass);
	v = ((gchar *) o) + sizeof (MonoObject);
	if (is_static) {
		mono_field_static_get_value (vtable, field, v);
	} else {
		mono_field_get_value (obj, field, v);
	}

	return o;
}

int
mono_get_constant_value_from_blob (MonoDomain* domain, MonoTypeEnum type, const char *blob, void *value)
{
	int retval = 0;
	const char *p = blob;
	mono_metadata_decode_blob_size (p, &p);

	switch (type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
		*(guint8 *) value = *p;
		break;
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
		*(guint16*) value = read16 (p);
		break;
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
		*(guint32*) value = read32 (p);
		break;
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
		*(guint64*) value = read64 (p);
		break;
	case MONO_TYPE_R4:
		readr4 (p, (float*) value);
		break;
	case MONO_TYPE_R8:
		readr8 (p, (double*) value);
		break;
	case MONO_TYPE_STRING:
		*(gpointer*) value = mono_ldstr_metdata_sig (domain, blob);
		break;
	case MONO_TYPE_CLASS:
		*(gpointer*) value = NULL;
		break;
	default:
		retval = -1;
		g_warning ("type 0x%02x should not be in constant table", type);
	}
	return retval;
}

static void
get_default_field_value (MonoDomain* domain, MonoClassField *field, void *value)
{
	g_return_if_fail (field->type->attrs & FIELD_ATTRIBUTE_HAS_DEFAULT);
	mono_get_constant_value_from_blob (domain, field->def_type, field->data, value);
}

/**
 * mono_field_static_get_value:
 * @vt: vtable to the object
 * @field: MonoClassField describing the field to fetch information from
 * @value: where the value is returned
 *
 * Use this routine to get the value of the static field @field value.
 *
 * The pointer provided by value must be of the field type, for reference
 * types this is a MonoObject*, for value types its the actual pointer to
 * the value type.
 *
 * For example:
 *     int i;
 *     mono_field_static_get_value (vt, int_field, &i);
 */
void
mono_field_static_get_value (MonoVTable *vt, MonoClassField *field, void *value)
{
	void *src;

	g_return_if_fail (field->type->attrs & FIELD_ATTRIBUTE_STATIC);
	
	if (field->type->attrs & FIELD_ATTRIBUTE_LITERAL) {
		get_default_field_value (vt->domain, field, value);
		return;
	}

	src = (char*)vt->data + field->offset;
	set_value (field->type, value, src, TRUE);
}

/**
 * mono_property_set_value:
 * @prop: MonoProperty to set
 * @obj: instance object on which to act
 * @params: parameters to pass to the propery
 * @exc: optional exception
 *
 * Invokes the property's set method with the given arguments on the
 * object instance obj (or NULL for static properties). 
 * 
 * You can pass NULL as the exc argument if you don't want to
 * catch exceptions, otherwise, *exc will be set to the exception
 * thrown, if any.  if an exception is thrown, you can't use the
 * MonoObject* result from the function.
 */
void
mono_property_set_value (MonoProperty *prop, void *obj, void **params, MonoObject **exc)
{
	default_mono_runtime_invoke (prop->set, obj, params, exc);
}

/**
 * mono_property_get_value:
 * @prop: MonoProperty to fetch
 * @obj: instance object on which to act
 * @params: parameters to pass to the propery
 * @exc: optional exception
 *
 * Invokes the property's get method with the given arguments on the
 * object instance obj (or NULL for static properties). 
 * 
 * You can pass NULL as the exc argument if you don't want to
 * catch exceptions, otherwise, *exc will be set to the exception
 * thrown, if any.  if an exception is thrown, you can't use the
 * MonoObject* result from the function.
 *
 * Returns: the value from invoking the get method on the property.
 */
MonoObject*
mono_property_get_value (MonoProperty *prop, void *obj, void **params, MonoObject **exc)
{
	return default_mono_runtime_invoke (prop->get, obj, params, exc);
}


/**
 * mono_get_delegate_invoke:
 * @klass: The delegate class
 *
 * Returns: the MonoMethod for the "Invoke" method in the delegate klass
 */
MonoMethod *
mono_get_delegate_invoke (MonoClass *klass)
{
	MonoMethod *im;

	im = mono_class_get_method_from_name (klass, "Invoke", -1);
	g_assert (im);

	return im;
}

/**
 * mono_runtime_delegate_invoke:
 * @delegate: pointer to a delegate object.
 * @params: parameters for the delegate.
 * @exc: Pointer to the exception result.
 *
 * Invokes the delegate method @delegate with the parameters provided.
 *
 * You can pass NULL as the exc argument if you don't want to
 * catch exceptions, otherwise, *exc will be set to the exception
 * thrown, if any.  if an exception is thrown, you can't use the
 * MonoObject* result from the function.
 */
MonoObject*
mono_runtime_delegate_invoke (MonoObject *delegate, void **params, MonoObject **exc)
{
	MonoMethod *im;

	im = mono_get_delegate_invoke (delegate->vtable->klass);
	g_assert (im);

	return mono_runtime_invoke (im, delegate, params, exc);
}

static char **main_args = NULL;
static int num_main_args;

/**
 * mono_runtime_get_main_args:
 *
 * Returns: a MonoArray with the arguments passed to the main program
 */
MonoArray*
mono_runtime_get_main_args (void)
{
	MonoArray *res;
	int i;
	MonoDomain *domain = mono_domain_get ();

	if (!main_args)
		return NULL;

	res = (MonoArray*)mono_array_new (domain, mono_defaults.string_class, num_main_args);

	for (i = 0; i < num_main_args; ++i)
		mono_array_set (res, gpointer, i, mono_string_new (domain, main_args [i]));

	return res;
}

static void
fire_process_exit_event (void)
{
	MonoClassField *field;
	MonoDomain *domain = mono_domain_get ();
	gpointer pa [2];
	MonoObject *delegate, *exc;
	
	field = mono_class_get_field_from_name (mono_defaults.appdomain_class, "ProcessExit");
	g_assert (field);

	if (domain != mono_get_root_domain ())
		return;

	delegate = *(MonoObject **)(((char *)domain->domain) + field->offset); 
	if (delegate == NULL)
		return;

	pa [0] = domain;
	pa [1] = NULL;
	mono_runtime_delegate_invoke (delegate, pa, &exc);
}

/**
 * mono_runtime_run_main:
 * @method: the method to start the application with (usually Main)
 * @argc: number of arguments from the command line
 * @argv: array of strings from the command line
 * @exc: excetption results
 *
 * Execute a standard Main() method (argc/argv contains the
 * executable name). This method also sets the command line argument value
 * needed by System.Environment.
 *
 * 
 */
int
mono_runtime_run_main (MonoMethod *method, int argc, char* argv[],
		       MonoObject **exc)
{
	int i;
	MonoArray *args = NULL;
	MonoDomain *domain = mono_domain_get ();
	gchar *utf8_fullpath;
	int result;

	mono_thread_set_main (mono_thread_current ());

	main_args = g_new0 (char*, argc);
	num_main_args = argc;

	if (!g_path_is_absolute (argv [0])) {
		gchar *basename = g_path_get_basename (argv [0]);
		gchar *fullpath = g_build_filename (method->klass->image->assembly->basedir,
						    basename,
						    NULL);

		utf8_fullpath = mono_utf8_from_external (fullpath);
		if(utf8_fullpath == NULL) {
			/* Printing the arg text will cause glib to
			 * whinge about "Invalid UTF-8", but at least
			 * its relevant, and shows the problem text
			 * string.
			 */
			g_print ("\nCannot determine the text encoding for the assembly location: %s\n", fullpath);
			g_print ("Please add the correct encoding to MONO_EXTERNAL_ENCODINGS and try again.\n");
			exit (-1);
		}

		g_free (fullpath);
		g_free (basename);
	} else {
		utf8_fullpath = mono_utf8_from_external (argv[0]);
		if(utf8_fullpath == NULL) {
			g_print ("\nCannot determine the text encoding for the assembly location: %s\n", argv[0]);
			g_print ("Please add the correct encoding to MONO_EXTERNAL_ENCODINGS and try again.\n");
			exit (-1);
		}
	}

	main_args [0] = utf8_fullpath;

	for (i = 1; i < argc; ++i) {
		gchar *utf8_arg;

		utf8_arg=mono_utf8_from_external (argv[i]);
		if(utf8_arg==NULL) {
			/* Ditto the comment about Invalid UTF-8 here */
			g_print ("\nCannot determine the text encoding for argument %d (%s).\n", i, argv[i]);
			g_print ("Please add the correct encoding to MONO_EXTERNAL_ENCODINGS and try again.\n");
			exit (-1);
		}

		main_args [i] = utf8_arg;
	}
	argc--;
	argv++;
	if (mono_method_signature (method)->param_count) {
		args = (MonoArray*)mono_array_new (domain, mono_defaults.string_class, argc);
		for (i = 0; i < argc; ++i) {
			/* The encodings should all work, given that
			 * we've checked all these args for the
			 * main_args array.
			 */
			gchar *str = mono_utf8_from_external (argv [i]);
			MonoString *arg = mono_string_new (domain, str);
			mono_array_set (args, gpointer, i, arg);
			g_free (str);
		}
	} else {
		args = (MonoArray*)mono_array_new (domain, mono_defaults.string_class, 0);
	}
	
	mono_assembly_set_main (method->klass->image->assembly);

	result = mono_runtime_exec_main (method, args, exc);
	fire_process_exit_event ();
	return result;
}

/* Used in mono_unhandled_exception */
static MonoObject *
create_unhandled_exception_eventargs (MonoObject *exc)
{
	MonoClass *klass;
	gpointer args [2];
	MonoMethod *method = NULL;
	MonoBoolean is_terminating = TRUE;
	MonoObject *obj;

	klass = mono_class_from_name (mono_defaults.corlib, "System", "UnhandledExceptionEventArgs");
	g_assert (klass);

	mono_class_init (klass);

	/* UnhandledExceptionEventArgs only has 1 public ctor with 2 args */
	method = mono_class_get_method_from_name_flags (klass, ".ctor", 2, METHOD_ATTRIBUTE_PUBLIC);
	g_assert (method);

	args [0] = exc;
	args [1] = &is_terminating;

	obj = mono_object_new (mono_domain_get (), klass);
	mono_runtime_invoke (method, obj, args, NULL);

	return obj;
}

/**
 * mono_unhandled_exception:
 * @exc: exception thrown
 *
 * This is a VM internal routine.
 *
 * We call this function when we detect an unhandled exception
 * in the default domain.
 *
 * It invokes the * UnhandledException event in AppDomain or prints
 * a warning to the console 
 */
void
mono_unhandled_exception (MonoObject *exc)
{
	MonoDomain *domain = mono_domain_get ();
	MonoClassField *field;
	MonoObject *delegate;

	field=mono_class_get_field_from_name(mono_defaults.appdomain_class, 
					     "UnhandledException");
	g_assert (field);

	if (exc->vtable->klass != mono_defaults.threadabortexception_class) {
		delegate = *(MonoObject **)(((char *)domain->domain) + field->offset); 

		/* set exitcode only in the main thread */
		if (mono_thread_current () == main_thread)
			mono_environment_exitcode_set (1);
		if (domain != mono_get_root_domain () || !delegate) {
			mono_print_unhandled_exception (exc);
		} else {
			MonoObject *e = NULL;
			gpointer pa [2];

			pa [0] = domain->domain;
			pa [1] = create_unhandled_exception_eventargs (exc);
			mono_runtime_delegate_invoke (delegate, pa, &e);
			
			if (e) {
				gchar *msg = mono_string_to_utf8 (((MonoException *) e)->message);
				g_warning ("exception inside UnhandledException handler: %s\n", msg);
				g_free (msg);
			}
		}
	}
}

/*
 * Launch a new thread to execute a function
 *
 * main_func is called back from the thread with main_args as the
 * parameter.  The callback function is expected to start Main()
 * eventually.  This function then waits for all managed threads to
 * finish.
 * It is not necesseray anymore to execute managed code in a subthread,
 * so this function should not be used anymore by default: just
 * execute the code and then call mono_thread_manage ().
 */
void
mono_runtime_exec_managed_code (MonoDomain *domain,
				MonoMainThreadFunc main_func,
				gpointer main_args)
{
	mono_thread_create (domain, main_func, main_args);

	mono_thread_manage ();
}

/*
 * Execute a standard Main() method (args doesn't contain the
 * executable name).
 */
int
mono_runtime_exec_main (MonoMethod *method, MonoArray *args, MonoObject **exc)
{
	MonoDomain *domain;
	gpointer pa [1];
	int rval;

	g_assert (args);

	pa [0] = args;

	domain = mono_object_domain (args);
	if (!domain->entry_assembly) {
		gchar *str;
		gchar *config_suffix;
		MonoAssembly *assembly;

		assembly = method->klass->image->assembly;
		domain->entry_assembly = assembly;
		domain->setup->application_base = mono_string_new (domain, assembly->basedir);

		config_suffix = g_strconcat (assembly->aname.name, ".exe.config", NULL);
		str = g_build_filename (assembly->basedir, config_suffix, NULL);
		g_free (config_suffix);
		domain->setup->configuration_file = mono_string_new (domain, str);
		g_free (str);
	}

	/* FIXME: check signature of method */
	if (mono_method_signature (method)->ret->type == MONO_TYPE_I4) {
		MonoObject *res;
		res = mono_runtime_invoke (method, NULL, pa, exc);
		if (!exc || !*exc)
			rval = *(guint32 *)((char *)res + sizeof (MonoObject));
		else
			rval = -1;

		mono_environment_exitcode_set (rval);
	} else {
		mono_runtime_invoke (method, NULL, pa, exc);
		if (!exc || !*exc)
			rval = 0;
		else {
			/* If the return type of Main is void, only
			 * set the exitcode if an exception was thrown
			 * (we don't want to blow away an
			 * explicitly-set exit code)
			 */
			rval = -1;
			mono_environment_exitcode_set (rval);
		}
	}

	return rval;
}

/**
 * mono_install_runtime_invoke:
 * @func: Function to install
 *
 * This is a VM internal routine
 */
void
mono_install_runtime_invoke (MonoInvokeFunc func)
{
	default_mono_runtime_invoke = func ? func: dummy_mono_runtime_invoke;
}

/**
 * mono_runtime_invoke_array:
 * @method: method to invoke
 * @obJ: object instance
 * @params: arguments to the method
 * @exc: exception information.
 *
 * Invokes the method represented by @method on the object @obj.
 *
 * obj is the 'this' pointer, it should be NULL for static
 * methods, a MonoObject* for object instances and a pointer to
 * the value type for value types.
 *
 * The params array contains the arguments to the method with the
 * same convention: MonoObject* pointers for object instances and
 * pointers to the value type otherwise. The _invoke_array
 * variant takes a C# object[] as the params argument (MonoArray
 * *params): in this case the value types are boxed inside the
 * respective reference representation.
 * 
 * From unmanaged code you'll usually use the
 * mono_runtime_invoke() variant.
 *
 * Note that this function doesn't handle virtual methods for
 * you, it will exec the exact method you pass: we still need to
 * expose a function to lookup the derived class implementation
 * of a virtual method (there are examples of this in the code,
 * though).
 * 
 * You can pass NULL as the exc argument if you don't want to
 * catch exceptions, otherwise, *exc will be set to the exception
 * thrown, if any.  if an exception is thrown, you can't use the
 * MonoObject* result from the function.
 * 
 * If the method returns a value type, it is boxed in an object
 * reference.
 */
MonoObject*
mono_runtime_invoke_array (MonoMethod *method, void *obj, MonoArray *params,
			   MonoObject **exc)
{
	MonoMethodSignature *sig = mono_method_signature (method);
	gpointer *pa = NULL;
	int i;
		
	if (NULL != params) {
		pa = alloca (sizeof (gpointer) * mono_array_length (params));
		for (i = 0; i < mono_array_length (params); i++) {
			if (sig->params [i]->byref) {
				/* nothing to do */
			}

			switch (sig->params [i]->type) {
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
			case MONO_TYPE_U8:
			case MONO_TYPE_I8:
			case MONO_TYPE_R4:
			case MONO_TYPE_R8:
			case MONO_TYPE_VALUETYPE:
				/* MS seems to create the objects if a null is passed in */
				if (! ((gpointer *)params->vector)[i])
					((gpointer*)params->vector)[i] = mono_object_new (mono_domain_get (), mono_class_from_mono_type (sig->params [i]));
				pa [i] = (char *)(((gpointer *)params->vector)[i]) + sizeof (MonoObject);
				break;
			case MONO_TYPE_STRING:
			case MONO_TYPE_OBJECT:
			case MONO_TYPE_CLASS:
			case MONO_TYPE_ARRAY:
			case MONO_TYPE_SZARRAY:
				if (sig->params [i]->byref)
					pa [i] = &(((gpointer *)params->vector)[i]);
				else
					pa [i] = (char *)(((gpointer *)params->vector)[i]);
				break;
			default:
				g_error ("type 0x%x not handled in ves_icall_InternalInvoke", sig->params [i]->type);
			}
		}
	}

	if (!strcmp (method->name, ".ctor") && method->klass != mono_defaults.string_class) {
		void *o = obj;
		if (!obj) {
			obj = mono_object_new (mono_domain_get (), method->klass);
			if (mono_object_class(obj) == mono_defaults.transparent_proxy_class) {
				method = mono_marshal_get_remoting_invoke (method->slot == -1 ? method : method->klass->vtable [method->slot]);
			}
			if (method->klass->valuetype)
				o = mono_object_unbox (obj);
			else
				o = obj;
		}
		else if (method->klass->valuetype)
			obj = mono_value_box (mono_domain_get (), method->klass, obj);

		mono_runtime_invoke (method, o, pa, exc);
		return obj;
	} else {
		/* obj must be already unboxed if needed */
		return mono_runtime_invoke (method, obj, pa, exc);
	}
}

static void
arith_overflow (void)
{
	mono_raise_exception (mono_get_exception_overflow ());
}

/**
 * mono_object_allocate:
 * @size: number of bytes to allocate
 *
 * This is a very simplistic routine until we have our GC-aware
 * memory allocator. 
 *
 * Returns: an allocated object of size @size, or NULL on failure.
 */
static inline void *
mono_object_allocate (size_t size, MonoVTable *vtable)
{
	MonoObject *o;
	mono_stats.new_object_count++;
	ALLOC_OBJECT (o, vtable, size);

	return o;
}

/**
 * mono_object_allocate_ptrfree:
 * @size: number of bytes to allocate
 *
 * Note that the memory allocated is not zeroed.
 * Returns: an allocated object of size @size, or NULL on failure.
 */
static inline void *
mono_object_allocate_ptrfree (size_t size, MonoVTable *vtable)
{
	MonoObject *o;
	mono_stats.new_object_count++;
	ALLOC_PTRFREE (o, vtable, size);
	return o;
}

static inline void *
mono_object_allocate_spec (size_t size, MonoVTable *vtable)
{
	void *o;
	ALLOC_TYPED (o, size, vtable);
	mono_stats.new_object_count++;

	return o;
}

/**
 * mono_object_new:
 * @klass: the class of the object that we want to create
 *
 * Returns: a newly created object whose definition is
 * looked up using @klass.   This will not invoke any constructors, 
 * so the consumer of this routine has to invoke any constructors on
 * its own to initialize the object.
 */
MonoObject *
mono_object_new (MonoDomain *domain, MonoClass *klass)
{
	MONO_ARCH_SAVE_REGS;
	return mono_object_new_specific (mono_class_vtable (domain, klass));
}

/**
 * mono_object_new_specific:
 * @vtable: the vtable of the object that we want to create
 *
 * Returns: A newly created object with class and domain specified
 * by @vtable
 */
MonoObject *
mono_object_new_specific (MonoVTable *vtable)
{
	MonoObject *o;

	MONO_ARCH_SAVE_REGS;
	
	if (vtable->remote)
	{
		gpointer pa [1];
		MonoMethod *im = vtable->domain->create_proxy_for_type_method;

		if (im == NULL) {
			MonoClass *klass = mono_class_from_name (mono_defaults.corlib, "System.Runtime.Remoting.Activation", "ActivationServices");

			if (!klass->inited)
				mono_class_init (klass);

			im = mono_class_get_method_from_name (klass, "CreateProxyForType", 1);
			g_assert (im);
			vtable->domain->create_proxy_for_type_method = im;
		}
	
		pa [0] = mono_type_get_object (mono_domain_get (), &vtable->klass->byval_arg);

		o = mono_runtime_invoke (im, NULL, pa, NULL);		
		if (o != NULL) return o;
	}

	return mono_object_new_alloc_specific (vtable);
}

MonoObject *
mono_object_new_alloc_specific (MonoVTable *vtable)
{
	MonoObject *o;

	if (!vtable->klass->has_references) {
		o = mono_object_new_ptrfree (vtable);
	} else if (vtable->gc_descr != GC_NO_DESCRIPTOR) {
		o = mono_object_allocate_spec (vtable->klass->instance_size, vtable);
	} else {
/*		printf("OBJECT: %s.%s.\n", vtable->klass->name_space, vtable->klass->name); */
		o = mono_object_allocate (vtable->klass->instance_size, vtable);
	}
	if (vtable->klass->has_finalize)
		mono_object_register_finalizer (o);
	
	mono_profiler_allocation (o, vtable->klass);
	return o;
}

MonoObject*
mono_object_new_fast (MonoVTable *vtable)
{
	MonoObject *o;
	ALLOC_TYPED (o, vtable->klass->instance_size, vtable);
	return o;
}

static MonoObject*
mono_object_new_ptrfree (MonoVTable *vtable)
{
	MonoObject *obj;
	ALLOC_PTRFREE (obj, vtable, vtable->klass->instance_size);
#if NEED_TO_ZERO_PTRFREE
	/* an inline memset is much faster for the common vcase of small objects
	 * note we assume the allocated size is a multiple of sizeof (void*).
	 */
	if (vtable->klass->instance_size < 128) {
		gpointer *p, *end;
		end = (gpointer*)((char*)obj + vtable->klass->instance_size);
		p = (gpointer*)((char*)obj + sizeof (MonoObject));
		while (p < end) {
			*p = NULL;
			++p;
		}
	} else {
		memset ((char*)obj + sizeof (MonoObject), 0, vtable->klass->instance_size - sizeof (MonoObject));
	}
#endif
	return obj;
}

static MonoObject*
mono_object_new_ptrfree_box (MonoVTable *vtable)
{
	MonoObject *obj;
	ALLOC_PTRFREE (obj, vtable, vtable->klass->instance_size);
	/* the object will be boxed right away, no need to memzero it */
	return obj;
}

/**
 * mono_class_get_allocation_ftn:
 * @vtable: vtable
 * @for_box: the object will be used for boxing
 * @pass_size_in_words: 
 *
 * Return the allocation function appropriate for the given class.
 */

void*
mono_class_get_allocation_ftn (MonoVTable *vtable, gboolean for_box, gboolean *pass_size_in_words)
{
	*pass_size_in_words = FALSE;

	if (vtable->klass->has_finalize || vtable->klass->marshalbyref || (mono_profiler_get_events () & MONO_PROFILE_ALLOCATIONS))
		return mono_object_new_specific;

	if (!vtable->klass->has_references) {
		//g_print ("ptrfree for %s.%s\n", vtable->klass->name_space, vtable->klass->name);
		if (for_box)
			return mono_object_new_ptrfree_box;
		return mono_object_new_ptrfree;
	}

	if (vtable->gc_descr != GC_NO_DESCRIPTOR) {

		return mono_object_new_fast;

		/* 
		 * FIXME: This is actually slower than mono_object_new_fast, because
		 * of the overhead of parameter passing.
		 */
		/*
		*pass_size_in_words = TRUE;
#ifdef GC_REDIRECT_TO_LOCAL
		return GC_local_gcj_fast_malloc;
#else
		return GC_gcj_fast_malloc;
#endif
		*/
	}

	return mono_object_new_specific;
}

/**
 * mono_object_new_from_token:
 * @image: Context where the type_token is hosted
 * @token: a token of the type that we want to create
 *
 * Returns: A newly created object whose definition is
 * looked up using @token in the @image image
 */
MonoObject *
mono_object_new_from_token  (MonoDomain *domain, MonoImage *image, guint32 token)
{
	MonoClass *class;

	class = mono_class_get (image, token);

	return mono_object_new (domain, class);
}


/**
 * mono_object_clone:
 * @obj: the object to clone
 *
 * Returns: A newly created object who is a shallow copy of @obj
 */
MonoObject *
mono_object_clone (MonoObject *obj)
{
	MonoObject *o;
	int size;

	size = obj->vtable->klass->instance_size;
	o = mono_object_allocate (size, obj->vtable);
	/* do not copy the sync state */
	memcpy ((char*)o + sizeof (MonoObject), (char*)obj + sizeof (MonoObject), size - sizeof (MonoObject));
	
	mono_profiler_allocation (o, obj->vtable->klass);

	if (obj->vtable->klass->has_finalize)
		mono_object_register_finalizer (o);
	return o;
}

/**
 * mono_array_full_copy:
 * @src: source array to copy
 * @dest: destination array
 *
 * Copies the content of one array to another with exactly the same type and size.
 */
void
mono_array_full_copy (MonoArray *src, MonoArray *dest)
{
	int size;
	MonoClass *klass = src->obj.vtable->klass;

	MONO_ARCH_SAVE_REGS;

	g_assert (klass == dest->obj.vtable->klass);

	size = mono_array_length (src);
	g_assert (size == mono_array_length (dest));
	size *= mono_array_element_size (klass);
	memcpy (&dest->vector, &src->vector, size);
}

/**
 * mono_array_clone_in_domain:
 * @domain: the domain in which the array will be cloned into
 * @array: the array to clone
 *
 * This routine returns a copy of the array that is hosted on the
 * specified MonoDomain.
 */
MonoArray*
mono_array_clone_in_domain (MonoDomain *domain, MonoArray *array)
{
	MonoArray *o;
	int size, i;
	guint32 *sizes;
	MonoClass *klass = array->obj.vtable->klass;

	MONO_ARCH_SAVE_REGS;

	if (array->bounds == NULL) {
		size = mono_array_length (array);
		o = mono_array_new_full (domain, klass, &size, NULL);

		size *= mono_array_element_size (klass);
		memcpy (&o->vector, &array->vector, size);
		return o;
	}
	
	sizes = alloca (klass->rank * sizeof(guint32) * 2);
	size = mono_array_element_size (klass);
	for (i = 0; i < klass->rank; ++i) {
		sizes [i] = array->bounds [i].length;
		size *= array->bounds [i].length;
		sizes [i + klass->rank] = array->bounds [i].lower_bound;
	}
	o = mono_array_new_full (domain, klass, sizes, sizes + klass->rank);
	memcpy (&o->vector, &array->vector, size);

	return o;
}

/**
 * mono_array_clone:
 * @array: the array to clone
 *
 * Returns: A newly created array who is a shallow copy of @array
 */
MonoArray*
mono_array_clone (MonoArray *array)
{
	return mono_array_clone_in_domain (((MonoObject *)array)->vtable->domain, array);
}

/* helper macros to check for overflow when calculating the size of arrays */
#define MYGUINT32_MAX 4294967295U
#define CHECK_ADD_OVERFLOW_UN(a,b) \
        (guint32)(MYGUINT32_MAX) - (guint32)(b) < (guint32)(a) ? -1 : 0
#define CHECK_MUL_OVERFLOW_UN(a,b) \
        ((guint32)(a) == 0) || ((guint32)(b) == 0) ? 0 : \
        (guint32)(b) > ((MYGUINT32_MAX) / (guint32)(a))

/**
 * mono_array_new_full:
 * @domain: domain where the object is created
 * @array_class: array class
 * @lengths: lengths for each dimension in the array
 * @lower_bounds: lower bounds for each dimension in the array (may be NULL)
 *
 * This routine creates a new array objects with the given dimensions,
 * lower bounds and type.
 */
MonoArray*
mono_array_new_full (MonoDomain *domain, MonoClass *array_class, 
		     guint32 *lengths, guint32 *lower_bounds)
{
	guint32 byte_len, len, bounds_size;
	MonoObject *o;
	MonoArray *array;
	MonoVTable *vtable;
	int i;

	if (!array_class->inited)
		mono_class_init (array_class);

	byte_len = mono_array_element_size (array_class);
	len = 1;

	if (array_class->rank == 1 &&
	    (lower_bounds == NULL || lower_bounds [0] == 0)) {
		len = lengths [0];
		if ((int) len < 0)
			arith_overflow ();
		bounds_size = 0;
	} else {
		bounds_size = sizeof (MonoArrayBounds) * array_class->rank;

		for (i = 0; i < array_class->rank; ++i) {
			if ((int) lengths [i] < 0)
				arith_overflow ();
			if (CHECK_MUL_OVERFLOW_UN (len, lengths [i]))
				mono_gc_out_of_memory (MYGUINT32_MAX);
			len *= lengths [i];
		}
	}

	if (CHECK_MUL_OVERFLOW_UN (byte_len, len))
		mono_gc_out_of_memory (MYGUINT32_MAX);
	byte_len *= len;
	if (CHECK_ADD_OVERFLOW_UN (byte_len, sizeof (MonoArray)))
		mono_gc_out_of_memory (MYGUINT32_MAX);
	byte_len += sizeof (MonoArray);
	if (bounds_size) {
		/* align */
		if (CHECK_ADD_OVERFLOW_UN (byte_len, 3))
			mono_gc_out_of_memory (MYGUINT32_MAX);
		byte_len = (byte_len + 3) & ~3;
		if (CHECK_ADD_OVERFLOW_UN (byte_len, bounds_size))
			mono_gc_out_of_memory (MYGUINT32_MAX);
		byte_len += bounds_size;
	}
	/* 
	 * Following three lines almost taken from mono_object_new ():
	 * they need to be kept in sync.
	 */
	vtable = mono_class_vtable (domain, array_class);
	if (!array_class->has_references) {
		o = mono_object_allocate_ptrfree (byte_len, vtable);
#if NEED_TO_ZERO_PTRFREE
		memset ((char*)o + sizeof (MonoObject), 0, byte_len - sizeof (MonoObject));
#endif
	} else if (vtable->gc_descr != GC_NO_DESCRIPTOR) {
		o = mono_object_allocate_spec (byte_len, vtable);
	}else {
		o = mono_object_allocate (byte_len, vtable);
	}

	array = (MonoArray*)o;
	array->max_length = len;

	if (bounds_size) {
		MonoArrayBounds *bounds = (MonoArrayBounds*)((char*)array + byte_len - bounds_size);
		array->bounds = bounds;
		for (i = 0; i < array_class->rank; ++i) {
			bounds [i].length = lengths [i];
			if (lower_bounds)
				bounds [i].lower_bound = lower_bounds [i];
		}
	}

	mono_profiler_allocation (o, array_class);

	return array;
}

/**
 * mono_array_new:
 * @domain: domain where the object is created
 * @eclass: element class
 * @n: number of array elements
 *
 * This routine creates a new szarray with @n elements of type @eclass.
 */
MonoArray *
mono_array_new (MonoDomain *domain, MonoClass *eclass, guint32 n)
{
	MonoClass *ac;

	MONO_ARCH_SAVE_REGS;

	ac = mono_array_class_get (eclass, 1);
	g_assert (ac != NULL);

	return mono_array_new_specific (mono_class_vtable (domain, ac), n);
}

/**
 * mono_array_new_specific:
 * @vtable: a vtable in the appropriate domain for an initialized class
 * @n: number of array elements
 *
 * This routine is a fast alternative to mono_array_new() for code which
 * can be sure about the domain it operates in.
 */
MonoArray *
mono_array_new_specific (MonoVTable *vtable, guint32 n)
{
	MonoObject *o;
	MonoArray *ao;
	guint32 byte_len, elem_size;

	MONO_ARCH_SAVE_REGS;

	if ((int) n < 0)
		arith_overflow ();
	
	elem_size = mono_array_element_size (vtable->klass);
	if (CHECK_MUL_OVERFLOW_UN (n, elem_size))
		mono_gc_out_of_memory (MYGUINT32_MAX);
	byte_len = n * elem_size;
	if (CHECK_ADD_OVERFLOW_UN (byte_len, sizeof (MonoArray)))
		mono_gc_out_of_memory (MYGUINT32_MAX);
	byte_len += sizeof (MonoArray);
	if (!vtable->klass->has_references) {
		o = mono_object_allocate_ptrfree (byte_len, vtable);
#if NEED_TO_ZERO_PTRFREE
		memset ((char*)o + sizeof (MonoObject), 0, byte_len - sizeof (MonoObject));
#endif
	} else if (vtable->gc_descr != GC_NO_DESCRIPTOR) {
		o = mono_object_allocate_spec (byte_len, vtable);
	} else {
/*		printf("ARRAY: %s.%s.\n", vtable->klass->name_space, vtable->klass->name); */
		o = mono_object_allocate (byte_len, vtable);
	}

	ao = (MonoArray *)o;
	ao->bounds = NULL;
	ao->max_length = n;
	mono_profiler_allocation (o, vtable->klass);

	return ao;
}

/**
 * mono_string_new_utf16:
 * @text: a pointer to an utf16 string
 * @len: the length of the string
 *
 * Returns: A newly created string object which contains @text.
 */
MonoString *
mono_string_new_utf16 (MonoDomain *domain, const guint16 *text, gint32 len)
{
	MonoString *s;
	
	s = mono_string_new_size (domain, len);
	g_assert (s != NULL);

	memcpy (mono_string_chars (s), text, len * 2);

	return s;
}

/**
 * mono_string_new_size:
 * @text: a pointer to an utf16 string
 * @len: the length of the string
 *
 * Returns: A newly created string object of @len
 */
MonoString *
mono_string_new_size (MonoDomain *domain, gint32 len)
{
	MonoString *s;
	MonoVTable *vtable;
	size_t size = (sizeof (MonoString) + ((len + 1) * 2));

	/* overflow ? can't fit it, can't allocate it! */
	if (len > size)
		mono_gc_out_of_memory (-1);

	vtable = mono_class_vtable (domain, mono_defaults.string_class);

	s = mono_object_allocate_ptrfree (size, vtable);

	s->length = len;
#if NEED_TO_ZERO_PTRFREE
	s->chars [len] = 0;
#endif
	mono_profiler_allocation ((MonoObject*)s, mono_defaults.string_class);

	return s;
}

/**
 * mono_string_new_len:
 * @text: a pointer to an utf8 string
 * @length: number of bytes in @text to consider
 *
 * Returns: A newly created string object which contains @text.
 */
MonoString*
mono_string_new_len (MonoDomain *domain, const char *text, guint length)
{
	GError *error = NULL;
	MonoString *o = NULL;
	guint16 *ut;
	glong items_written;

	ut = g_utf8_to_utf16 (text, length, NULL, &items_written, &error);

	if (!error)
		o = mono_string_new_utf16 (domain, ut, items_written);
	else 
		g_error_free (error);

	g_free (ut);

	return o;
}

/**
 * mono_string_new:
 * @text: a pointer to an utf8 string
 *
 * Returns: A newly created string object which contains @text.
 */
MonoString*
mono_string_new (MonoDomain *domain, const char *text)
{
	GError *error = NULL;
	MonoString *o = NULL;
	guint16 *ut;
	glong items_written;
	int l;

	l = strlen (text);
	
	ut = g_utf8_to_utf16 (text, l, NULL, &items_written, &error);

	if (!error)
		o = mono_string_new_utf16 (domain, ut, items_written);
	else 
		g_error_free (error);

	g_free (ut);

	return o;
}

/**
 * mono_string_new_wrapper:
 * @text: pointer to utf8 characters.
 *
 * Helper function to create a string object from @text in the current domain.
 */
MonoString*
mono_string_new_wrapper (const char *text)
{
	MonoDomain *domain = mono_domain_get ();

	MONO_ARCH_SAVE_REGS;

	if (text)
		return mono_string_new (domain, text);

	return NULL;
}

/**
 * mono_value_box:
 * @class: the class of the value
 * @value: a pointer to the unboxed data
 *
 * Returns: A newly created object which contains @value.
 */
MonoObject *
mono_value_box (MonoDomain *domain, MonoClass *class, gpointer value)
{
	MonoObject *res;
	int size;
	MonoVTable *vtable;

	g_assert (class->valuetype);

	vtable = mono_class_vtable (domain, class);
	size = mono_class_instance_size (class);
	res = mono_object_allocate (size, vtable);
	mono_profiler_allocation (res, class);

	size = size - sizeof (MonoObject);

#if NO_UNALIGNED_ACCESS
	memcpy ((char *)res + sizeof (MonoObject), value, size);
#else
	switch (size) {
	case 1:
		*((guint8 *) res + sizeof (MonoObject)) = *(guint8 *) value;
		break;
	case 2:
		*(guint16 *)((guint8 *) res + sizeof (MonoObject)) = *(guint16 *) value;
		break;
	case 4:
		*(guint32 *)((guint8 *) res + sizeof (MonoObject)) = *(guint32 *) value;
		break;
	case 8:
		*(guint64 *)((guint8 *) res + sizeof (MonoObject)) = *(guint64 *) value;
		break;
	default:
		memcpy ((char *)res + sizeof (MonoObject), value, size);
	}
#endif
	if (class->has_finalize)
		mono_object_register_finalizer (res);
	return res;
}

/**
 * mono_object_get_domain:
 * @obj: object to query
 * 
 * Returns: the MonoDomain where the object is hosted
 */
MonoDomain*
mono_object_get_domain (MonoObject *obj)
{
	return mono_object_domain (obj);
}

/**
 * mono_object_get_class:
 * @obj: object to query
 * 
 * Returns: the MonOClass of the object.
 */
MonoClass*
mono_object_get_class (MonoObject *obj)
{
	return mono_object_class (obj);
}
/**
 * mono_object_get_size:
 * @o: object to query
 * 
 * Returns: the size, in bytes, of @o
 */
guint
mono_object_get_size (MonoObject* o)
{
	MonoClass* klass = mono_object_class (o);
	
	if (klass == mono_defaults.string_class)
		return sizeof (MonoString) + 2 * mono_string_length ((MonoString*) o) + 2;
	else if (klass->parent == mono_defaults.array_class)
		return sizeof (MonoArray) + mono_array_element_size (klass) * mono_array_length ((MonoArray*) o);
	else
		return mono_class_instance_size (klass);
}

/**
 * mono_object_unbox:
 * @obj: object to unbox
 * 
 * Returns: a pointer to the start of the valuetype boxed in this
 * object.
 *
 * This method will assert if the object passed is not a valuetype.
 */
gpointer
mono_object_unbox (MonoObject *obj)
{
	/* add assert for valuetypes? */
	g_assert (obj->vtable->klass->valuetype);
	return ((char*)obj) + sizeof (MonoObject);
}

/**
 * mono_object_isinst:
 * @obj: an object
 * @klass: a pointer to a class 
 *
 * Returns: @obj if @obj is derived from @klass
 */
MonoObject *
mono_object_isinst (MonoObject *obj, MonoClass *klass)
{
	if (!klass->inited)
		mono_class_init (klass);

	if (klass->marshalbyref || klass->flags & TYPE_ATTRIBUTE_INTERFACE) 
		return mono_object_isinst_mbyref (obj, klass);

	if (!obj)
		return NULL;

	return mono_class_is_assignable_from (klass, obj->vtable->klass) ? obj : NULL;
}

MonoObject *
mono_object_isinst_mbyref (MonoObject *obj, MonoClass *klass)
{
	MonoVTable *vt;

	if (!obj)
		return NULL;

	vt = obj->vtable;
	
	if (klass->flags & TYPE_ATTRIBUTE_INTERFACE) {
		if ((klass->interface_id <= vt->max_interface_id) &&
		    (vt->interface_offsets [klass->interface_id] != 0))
			return obj;
	}
	else {
		MonoClass *oklass = vt->klass;
		if ((oklass == mono_defaults.transparent_proxy_class))
			oklass = ((MonoTransparentProxy *)obj)->remote_class->proxy_class;
	
		if ((oklass->idepth >= klass->idepth) && (oklass->supertypes [klass->idepth - 1] == klass))
			return obj;
	}

	if (vt->klass == mono_defaults.transparent_proxy_class && ((MonoTransparentProxy *)obj)->custom_type_info) 
	{
		MonoDomain *domain = mono_domain_get ();
		MonoObject *res;
		MonoObject *rp = (MonoObject *)((MonoTransparentProxy *)obj)->rp;
		MonoClass *rpklass = mono_defaults.iremotingtypeinfo_class;
		MonoMethod *im = NULL;
		gpointer pa [2];

		im = mono_class_get_method_from_name (rpklass, "CanCastTo", -1);
		im = mono_object_get_virtual_method (rp, im);
		g_assert (im);
	
		pa [0] = mono_type_get_object (domain, &klass->byval_arg);
		pa [1] = obj;

		res = mono_runtime_invoke (im, rp, pa, NULL);
	
		if (*(MonoBoolean *) mono_object_unbox(res)) {
			/* Update the vtable of the remote type, so it can safely cast to this new type */
			mono_upgrade_remote_class (domain, ((MonoTransparentProxy *)obj)->remote_class, klass);
			obj->vtable = mono_remote_class_vtable (domain, ((MonoTransparentProxy *)obj)->remote_class, (MonoRealProxy *)rp);
			return obj;
		}
	}

	return NULL;
}

/**
 * mono_object_castclass_mbyref:
 * @obj: an object
 * @klass: a pointer to a class 
 *
 * Returns: @obj if @obj is derived from @klass, throws an exception otherwise
 */
MonoObject *
mono_object_castclass_mbyref (MonoObject *obj, MonoClass *klass)
{
	if (!obj) return NULL;
	if (mono_object_isinst_mbyref (obj, klass)) return obj;
		
	mono_raise_exception (mono_exception_from_name (mono_defaults.corlib,
							"System",
							"InvalidCastException"));
	return NULL;
}

typedef struct {
	MonoDomain *orig_domain;
	MonoString *ins;
	MonoString *res;
} LDStrInfo;

static void
str_lookup (MonoDomain *domain, gpointer user_data)
{
	LDStrInfo *info = user_data;
	if (info->res || domain == info->orig_domain)
		return;
	mono_domain_lock (domain);
	info->res = mono_g_hash_table_lookup (domain->ldstr_table, info->ins);
	mono_domain_unlock (domain);
}

static MonoString*
mono_string_is_interned_lookup (MonoString *str, int insert)
{
	MonoGHashTable *ldstr_table;
	MonoString *res;
	MonoDomain *domain;
	
	domain = ((MonoObject *)str)->vtable->domain;
	ldstr_table = domain->ldstr_table;
	mono_domain_lock (domain);
	if ((res = mono_g_hash_table_lookup (ldstr_table, str))) {
		mono_domain_unlock (domain);
		return res;
	}
	if (insert) {
		mono_g_hash_table_insert (ldstr_table, str, str);
		mono_domain_unlock (domain);
		return str;
	} else {
		LDStrInfo ldstr_info;
		ldstr_info.orig_domain = domain;
		ldstr_info.ins = str;
		ldstr_info.res = NULL;

		mono_domain_foreach (str_lookup, &ldstr_info);
		if (ldstr_info.res) {
			/* 
			 * the string was already interned in some other domain:
			 * intern it in the current one as well.
			 */
			mono_g_hash_table_insert (ldstr_table, str, str);
			mono_domain_unlock (domain);
			return str;
		}
	}
	mono_domain_unlock (domain);
	return NULL;
}

/**
 * mono_string_is_interned:
 * @o: String to probe
 *
 * Returns whether the string has been interned.
 */
MonoString*
mono_string_is_interned (MonoString *o)
{
	return mono_string_is_interned_lookup (o, FALSE);
}

/**
 * mono_string_interne:
 * @o: String to intern
 *
 * Interns the string passed.  
 * Returns: The interned string.
 */
MonoString*
mono_string_intern (MonoString *str)
{
	return mono_string_is_interned_lookup (str, TRUE);
}

/**
 * mono_ldstr:
 * @domain: the domain where the string will be used.
 * @image: a metadata context
 * @idx: index into the user string table.
 * 
 * Implementation for the ldstr opcode.
 * Returns: a loaded string from the @image/@idx combination.
 */
MonoString*
mono_ldstr (MonoDomain *domain, MonoImage *image, guint32 idx)
{
	MONO_ARCH_SAVE_REGS;

	if (image->dynamic)
		return mono_lookup_dynamic_token (image, MONO_TOKEN_STRING | idx);
	else
		return mono_ldstr_metdata_sig (domain, mono_metadata_user_string (image, idx));
}

/**
 * mono_ldstr_metdata_sig
 * @domain: the domain for the string
 * @sig: the signature of a metadata string
 *
 * Returns: a MonoString for a string stored in the metadata
 */
static MonoString*
mono_ldstr_metdata_sig (MonoDomain *domain, const char* sig)
{
	const char *str = sig;
	MonoString *o, *interned;
	size_t len2;
	
	len2 = mono_metadata_decode_blob_size (str, &str);
	len2 >>= 1;

	o = mono_string_new_utf16 (domain, (guint16*)str, len2);
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	{
		int i;
		guint16 *p2 = (guint16*)mono_string_chars (o);
		for (i = 0; i < len2; ++i) {
			*p2 = GUINT16_FROM_LE (*p2);
			++p2;
		}
	}
#endif
	mono_domain_lock (domain);
	if ((interned = mono_g_hash_table_lookup (domain->ldstr_table, o))) {
		mono_domain_unlock (domain);
		/* o will get garbage collected */
		return interned;
	}

	mono_g_hash_table_insert (domain->ldstr_table, o, o);
	mono_domain_unlock (domain);

	return o;
}

/**
 * mono_string_to_utf8:
 * @s: a System.String
 *
 * Return the UTF8 representation for @s.
 * the resulting buffer nedds to be freed with g_free().
 */
char *
mono_string_to_utf8 (MonoString *s)
{
	char *as;
	GError *error = NULL;

	if (s == NULL)
		return NULL;

	if (!s->length)
		return g_strdup ("");

	as = g_utf16_to_utf8 (mono_string_chars (s), s->length, NULL, NULL, &error);
	if (error) {
		g_warning (error->message);
		g_error_free (error);
	}

	return as;
}

/**
 * mono_string_to_utf16:
 * @s: a MonoString
 *
 * Return an null-terminated array of the utf-16 chars
 * contained in @s. The result must be freed with g_free().
 * This is a temporary helper until our string implementation
 * is reworked to always include the null terminating char.
 */
gunichar2 *
mono_string_to_utf16 (MonoString *s)
{
	char *as;

	if (s == NULL)
		return NULL;

	as = g_malloc ((s->length * 2) + 2);
	as [(s->length * 2)] = '\0';
	as [(s->length * 2) + 1] = '\0';

	if (!s->length) {
		return (gunichar2 *)(as);
	}
	
	memcpy (as, mono_string_chars(s), s->length * 2);
	return (gunichar2 *)(as);
}

/**
 * mono_string_from_utf16:
 * @data: the UTF16 string (LPWSTR) to convert
 *
 * Converts a NULL terminated UTF16 string (LPWSTR) to a MonoString.
 *
 * Returns: a MonoString.
 */
MonoString *
mono_string_from_utf16 (gunichar2 *data)
{
	MonoDomain *domain = mono_domain_get ();
	int len = 0;

	if (!data)
		return NULL;

	while (data [len]) len++;

	return mono_string_new_utf16 (domain, data, len);
}

static void
default_ex_handler (MonoException *ex)
{
	MonoObject *o = (MonoObject*)ex;
	g_error ("Exception %s.%s raised in C code", o->vtable->klass->name_space, o->vtable->klass->name);
	exit (1);
}

static MonoExceptionFunc ex_handler = default_ex_handler;

/**
 * mono_install_handler:
 * @func: exception handler
 *
 * This is an internal JIT routine used to install the handler for exceptions
 * being throwh.
 */
void
mono_install_handler (MonoExceptionFunc func)
{
	ex_handler = func? func: default_ex_handler;
}

/**
 * mono_raise_exception:
 * @ex: exception object
 *
 * Signal the runtime that the exception @ex has been raised in unmanaged code.
 */
void
mono_raise_exception (MonoException *ex) 
{
	/*
	 * NOTE: Do NOT annotate this function with G_GNUC_NORETURN, since
	 * that will cause gcc to omit the function epilog, causing problems when
	 * the JIT tries to walk the stack, since the return address on the stack
	 * will point into the next function in the executable, not this one.
	 */

	if (((MonoObject*)ex)->vtable->klass == mono_defaults.threadabortexception_class)
		mono_thread_current ()->abort_exc = ex;
	
	ex_handler (ex);
}

/**
 * mono_wait_handle_new:
 * @domain: Domain where the object will be created
 * @handle: Handle for the wait handle
 *
 * Returns: A new MonoWaitHandle created in the given domain for the given handle
 */
MonoWaitHandle *
mono_wait_handle_new (MonoDomain *domain, HANDLE handle)
{
	MonoWaitHandle *res;

	res = (MonoWaitHandle *)mono_object_new (domain, mono_defaults.waithandle_class);

	res->handle = handle;

	return res;
}

/**
 * mono_async_result_new:
 * @domain:domain where the object will be created.
 * @handle: wait handle.
 * @state: state to pass to AsyncResult
 * @data: C closure data.
 *
 * Creates a new MonoAsyncResult (AsyncResult C# class) in the given domain.
 * If the handle is not null, the handle is initialized to a MonOWaitHandle.
 *
 */
MonoAsyncResult *
mono_async_result_new (MonoDomain *domain, HANDLE handle, MonoObject *state, gpointer data)
{
	MonoAsyncResult *res;

	res = (MonoAsyncResult *)mono_object_new (domain, mono_defaults.asyncresult_class);

	res->data = data;
	res->async_state = state;
	if (handle != NULL)
		res->handle = (MonoObject *) mono_wait_handle_new (domain, handle);

	res->sync_completed = FALSE;
	res->completed = FALSE;

	return res;
}

void
mono_message_init (MonoDomain *domain,
		   MonoMethodMessage *this, 
		   MonoReflectionMethod *method,
		   MonoArray *out_args)
{
	MonoMethodSignature *sig = mono_method_signature (method->method);
	MonoString *name;
	int i, j;
	char **names;
	guint8 arg_type;

	this->method = method;

	this->args = mono_array_new (domain, mono_defaults.object_class, sig->param_count);
	this->arg_types = mono_array_new (domain, mono_defaults.byte_class, sig->param_count);
	this->async_result = NULL;
	this->call_type = CallType_Sync;

	names = g_new (char *, sig->param_count);
	mono_method_get_param_names (method->method, (const char **) names);
	this->names = mono_array_new (domain, mono_defaults.string_class, sig->param_count);
	
	for (i = 0; i < sig->param_count; i++) {
		 name = mono_string_new (domain, names [i]);
		 mono_array_set (this->names, gpointer, i, name);	
	}

	g_free (names);
	for (i = 0, j = 0; i < sig->param_count; i++) {

		if (sig->params [i]->byref) {
			if (out_args) {
				gpointer arg = mono_array_get (out_args, gpointer, j);
				mono_array_set (this->args, gpointer, i, arg);
				j++;
			}
			arg_type = 2;
			if (!(sig->params [i]->attrs & PARAM_ATTRIBUTE_OUT))
				arg_type |= 1;
		} else {
			arg_type = 1;
		}
		mono_array_set (this->arg_types, guint8, i, arg_type);
	}
}

/**
 * mono_remoting_invoke:
 * @real_proxy: pointer to a RealProxy object
 * @msg: The MonoMethodMessage to execute
 * @exc: used to store exceptions
 * @out_args: used to store output arguments
 *
 * This is used to call RealProxy::Invoke(). RealProxy::Invoke() returns an
 * IMessage interface and it is not trivial to extract results from there. So
 * we call an helper method PrivateInvoke instead of calling
 * RealProxy::Invoke() directly.
 *
 * Returns: the result object.
 */
MonoObject *
mono_remoting_invoke (MonoObject *real_proxy, MonoMethodMessage *msg, 
		      MonoObject **exc, MonoArray **out_args)
{
	MonoMethod *im = real_proxy->vtable->domain->private_invoke_method;
	gpointer pa [4];

	/*static MonoObject *(*invoke) (gpointer, gpointer, MonoObject **, MonoArray **) = NULL;*/

	if (!im) {
		im = mono_class_get_method_from_name (mono_defaults.real_proxy_class, "PrivateInvoke", 4);
		g_assert (im);
		real_proxy->vtable->domain->private_invoke_method = im;
	}

	pa [0] = real_proxy;
	pa [1] = msg;
	pa [2] = exc;
	pa [3] = out_args;

	return mono_runtime_invoke (im, NULL, pa, exc);
}

MonoObject *
mono_message_invoke (MonoObject *target, MonoMethodMessage *msg, 
		     MonoObject **exc, MonoArray **out_args) 
{
	MonoDomain *domain; 
	MonoMethod *method;
	MonoMethodSignature *sig;
	MonoObject *ret;
	int i, j, outarg_count = 0;

	if (target && target->vtable->klass == mono_defaults.transparent_proxy_class) {

		MonoTransparentProxy* tp = (MonoTransparentProxy *)target;
		if (tp->remote_class->proxy_class->contextbound && tp->rp->context == (MonoObject *) mono_context_get ()) {
			target = tp->rp->unwrapped_server;
		} else {
			return mono_remoting_invoke ((MonoObject *)tp->rp, msg, exc, out_args);
		}
	}

	domain = mono_domain_get (); 
	method = msg->method->method;
	sig = mono_method_signature (method);

	for (i = 0; i < sig->param_count; i++) {
		if (sig->params [i]->byref) 
			outarg_count++;
	}

	*out_args = mono_array_new (domain, mono_defaults.object_class, outarg_count);
	*exc = NULL;

	ret = mono_runtime_invoke_array (method, method->klass->valuetype? mono_object_unbox (target): target, msg->args, exc);

	for (i = 0, j = 0; i < sig->param_count; i++) {
		if (sig->params [i]->byref) {
			gpointer arg;
			arg = mono_array_get (msg->args, gpointer, i);
			mono_array_set (*out_args, gpointer, j, arg);
			j++;
		}
	}

	return ret;
}

/**
 * mono_print_unhandled_exception:
 * @exc: The exception
 *
 * Prints the unhandled exception.
 */
void
mono_print_unhandled_exception (MonoObject *exc)
{
	char *message = (char *) "";
	MonoString *str; 
	MonoMethod *method;
	MonoClass *klass;
	gboolean free_message = FALSE;

	if (mono_object_isinst (exc, mono_defaults.exception_class)) {
		klass = exc->vtable->klass;
		method = NULL;
		while (klass && method == NULL) {
			method = mono_class_get_method_from_name_flags (klass, "ToString", 0, METHOD_ATTRIBUTE_VIRTUAL | METHOD_ATTRIBUTE_PUBLIC);
			if (method == NULL)
				klass = klass->parent;
		}

		g_assert (method);

		str = (MonoString *) mono_runtime_invoke (method, exc, NULL, NULL);
		if (str) {
			message = mono_string_to_utf8 (str);
			free_message = TRUE;
		}
	}				

	/*
	 * g_printerr ("\nUnhandled Exception: %s.%s: %s\n", exc->vtable->klass->name_space, 
	 *	   exc->vtable->klass->name, message);
	 */
	g_printerr ("\nUnhandled Exception: %s\n", message);
	
	if (free_message)
		g_free (message);
}

/**
 * mono_delegate_ctor:
 * @this: pointer to an uninitialized delegate object
 * @target: target object
 * @addr: pointer to native code
 *
 * This is used to initialize a delegate. We also insert the method_info if
 * we find the info with mono_jit_info_table_find().
 */
void
mono_delegate_ctor (MonoObject *this, MonoObject *target, gpointer addr)
{
	MonoDomain *domain = mono_domain_get ();
	MonoDelegate *delegate = (MonoDelegate *)this;
	MonoMethod *method = NULL;
	MonoClass *class;
	MonoJitInfo *ji;

	g_assert (this);
	g_assert (addr);

	class = this->vtable->klass;

	if ((ji = mono_jit_info_table_find (domain, addr))) {
		method = ji->method;
		delegate->method_info = mono_method_get_object (domain, method, NULL);
	}

	if (target && target->vtable->klass == mono_defaults.transparent_proxy_class) {
		g_assert (method);
		method = mono_marshal_get_remoting_invoke (method);
		delegate->method_ptr = mono_compile_method (method);
		delegate->target = target;
	} else if (mono_method_signature (method)->hasthis && method->klass->valuetype) {
		method = mono_marshal_get_unbox_wrapper (method);
		delegate->method_ptr = mono_compile_method (method);
		delegate->target = target;
	} else {
		delegate->method_ptr = addr;
		delegate->target = target;
	}
}

/**
 * mono_method_call_message_new:
 * @method: method to encapsulate
 * @params: parameters to the method
 * @invoke: optional, delegate invoke.
 * @cb: async callback delegate.
 * @state: state passed to the async callback.
 *
 * Translates arguments pointers into a MonoMethodMessage.
 */
MonoMethodMessage *
mono_method_call_message_new (MonoMethod *method, gpointer *params, MonoMethod *invoke, 
			      MonoDelegate **cb, MonoObject **state)
{
	MonoDomain *domain = mono_domain_get ();
	MonoMethodSignature *sig = mono_method_signature (method);
	MonoMethodMessage *msg;
	int i, count, type;

	msg = (MonoMethodMessage *)mono_object_new (domain, mono_defaults.mono_method_message_class); 
	
	if (invoke) {
		mono_message_init (domain, msg, mono_method_get_object (domain, invoke, NULL), NULL);
		count =  sig->param_count - 2;
	} else {
		mono_message_init (domain, msg, mono_method_get_object (domain, method, NULL), NULL);
		count =  sig->param_count;
	}

	for (i = 0; i < count; i++) {
		gpointer vpos;
		MonoClass *class;
		MonoObject *arg;

		if (sig->params [i]->byref)
			vpos = *((gpointer *)params [i]);
		else 
			vpos = params [i];

		type = sig->params [i]->type;
		class = mono_class_from_mono_type (sig->params [i]);

		if (class->valuetype)
			arg = mono_value_box (domain, class, vpos);
		else 
			arg = *((MonoObject **)vpos);
		      
		mono_array_set (msg->args, gpointer, i, arg);
	}

	if (cb != NULL && state != NULL) {
		*cb = *((MonoDelegate **)params [i]);
		i++;
		*state = *((MonoObject **)params [i]);
	}

	return msg;
}

/**
 * mono_method_return_message_restore:
 *
 * Restore results from message based processing back to arguments pointers
 */
void
mono_method_return_message_restore (MonoMethod *method, gpointer *params, MonoArray *out_args)
{
	MonoMethodSignature *sig = mono_method_signature (method);
	int i, j, type, size;
	for (i = 0, j = 0; i < sig->param_count; i++) {
		MonoType *pt = sig->params [i];

		if (pt->byref) {
			char *arg = mono_array_get (out_args, gpointer, j);
			type = pt->type;

			switch (type) {
			case MONO_TYPE_VOID:
				g_assert_not_reached ();
				break;
			case MONO_TYPE_U1:
			case MONO_TYPE_I1:
			case MONO_TYPE_BOOLEAN:
			case MONO_TYPE_U2:
			case MONO_TYPE_I2:
			case MONO_TYPE_CHAR:
			case MONO_TYPE_U4:
			case MONO_TYPE_I4:
			case MONO_TYPE_I8:
			case MONO_TYPE_U8:
			case MONO_TYPE_R4:
			case MONO_TYPE_R8:
			case MONO_TYPE_VALUETYPE: {
				size = mono_class_value_size (((MonoObject*)arg)->vtable->klass, NULL);
				memcpy (*((gpointer *)params [i]), arg + sizeof (MonoObject), size); 
				break;
			}
			case MONO_TYPE_STRING:
			case MONO_TYPE_CLASS: 
			case MONO_TYPE_ARRAY:
			case MONO_TYPE_SZARRAY:
			case MONO_TYPE_OBJECT:
				**((MonoObject ***)params [i]) = (MonoObject *)arg;
				break;
			default:
				g_assert_not_reached ();
			}

			j++;
		}
	}
}

/**
 * mono_load_remote_field:
 * @this: pointer to an object
 * @klass: klass of the object containing @field
 * @field: the field to load
 * @res: a storage to store the result
 *
 * This method is called by the runtime on attempts to load fields of
 * transparent proxy objects. @this points to such TP, @klass is the class of
 * the object containing @field. @res is a storage location which can be
 * used to store the result.
 *
 * Returns: an address pointing to the value of field.
 */
gpointer
mono_load_remote_field (MonoObject *this, MonoClass *klass, MonoClassField *field, gpointer *res)
{
	static MonoMethod *getter = NULL;
	MonoDomain *domain = mono_domain_get ();
	MonoTransparentProxy *tp = (MonoTransparentProxy *) this;
	MonoClass *field_class;
	MonoMethodMessage *msg;
	MonoArray *out_args;
	MonoObject *exc;
	gpointer tmp;

	g_assert (this->vtable->klass == mono_defaults.transparent_proxy_class);

	if (!res)
		res = &tmp;

	if (tp->remote_class->proxy_class->contextbound && tp->rp->context == (MonoObject *) mono_context_get ()) {
		mono_field_get_value (tp->rp->unwrapped_server, field, res);
		return res;
	}
	
	if (!getter) {
		getter = mono_class_get_method_from_name (mono_defaults.object_class, "FieldGetter", -1);
		g_assert (getter);
	}
	
	field_class = mono_class_from_mono_type (field->type);

	msg = (MonoMethodMessage *)mono_object_new (domain, mono_defaults.mono_method_message_class);
	out_args = mono_array_new (domain, mono_defaults.object_class, 1);
	mono_message_init (domain, msg, mono_method_get_object (domain, getter, NULL), out_args);

	mono_array_set (msg->args, gpointer, 0, mono_string_new (domain, klass->name));
	mono_array_set (msg->args, gpointer, 1, mono_string_new (domain, field->name));

	mono_remoting_invoke ((MonoObject *)(tp->rp), msg, &exc, &out_args);

	if (exc) mono_raise_exception ((MonoException *)exc);

	if (mono_array_length (out_args) == 0)
		return NULL;

	*res = mono_array_get (out_args, MonoObject *, 0);

	if (field_class->valuetype) {
		return ((char *)*res) + sizeof (MonoObject);
	} else
		return res;
}

/**
 * mono_load_remote_field_new:
 * @this: 
 * @klass: 
 * @field:
 *
 * Missing documentation.
 */
MonoObject *
mono_load_remote_field_new (MonoObject *this, MonoClass *klass, MonoClassField *field)
{
	static MonoMethod *getter = NULL;
	MonoDomain *domain = mono_domain_get ();
	MonoTransparentProxy *tp = (MonoTransparentProxy *) this;
	MonoClass *field_class;
	MonoMethodMessage *msg;
	MonoArray *out_args;
	MonoObject *exc, *res;

	g_assert (this->vtable->klass == mono_defaults.transparent_proxy_class);

	field_class = mono_class_from_mono_type (field->type);

	if (tp->remote_class->proxy_class->contextbound && tp->rp->context == (MonoObject *) mono_context_get ()) {
		gpointer val;
		if (field_class->valuetype) {
			res = mono_object_new (domain, field_class);
			val = ((gchar *) res) + sizeof (MonoObject);
		} else {
			val = &res;
		}
		mono_field_get_value (tp->rp->unwrapped_server, field, val);
		return res;
	}

	if (!getter) {
		getter = mono_class_get_method_from_name (mono_defaults.object_class, "FieldGetter", -1);
		g_assert (getter);
	}
	
	msg = (MonoMethodMessage *)mono_object_new (domain, mono_defaults.mono_method_message_class);
	out_args = mono_array_new (domain, mono_defaults.object_class, 1);

	mono_message_init (domain, msg, mono_method_get_object (domain, getter, NULL), out_args);

	mono_array_set (msg->args, gpointer, 0, mono_string_new (domain, klass->name));
	mono_array_set (msg->args, gpointer, 1, mono_string_new (domain, field->name));

	mono_remoting_invoke ((MonoObject *)(tp->rp), msg, &exc, &out_args);

	if (exc) mono_raise_exception ((MonoException *)exc);

	if (mono_array_length (out_args) == 0)
		res = NULL;
	else
		res = mono_array_get (out_args, MonoObject *, 0);

	return res;
}

/**
 * mono_store_remote_field:
 * @this: pointer to an object
 * @klass: klass of the object containing @field
 * @field: the field to load
 * @val: the value/object to store
 *
 * This method is called by the runtime on attempts to store fields of
 * transparent proxy objects. @this points to such TP, @klass is the class of
 * the object containing @field. @val is the new value to store in @field.
 */
void
mono_store_remote_field (MonoObject *this, MonoClass *klass, MonoClassField *field, gpointer val)
{
	static MonoMethod *setter = NULL;
	MonoDomain *domain = mono_domain_get ();
	MonoTransparentProxy *tp = (MonoTransparentProxy *) this;
	MonoClass *field_class;
	MonoMethodMessage *msg;
	MonoArray *out_args;
	MonoObject *exc;
	MonoObject *arg;

	g_assert (this->vtable->klass == mono_defaults.transparent_proxy_class);

	field_class = mono_class_from_mono_type (field->type);

	if (tp->remote_class->proxy_class->contextbound && tp->rp->context == (MonoObject *) mono_context_get ()) {
		if (field_class->valuetype) mono_field_set_value (tp->rp->unwrapped_server, field, val);
		else mono_field_set_value (tp->rp->unwrapped_server, field, *((MonoObject **)val));
		return;
	}

	if (!setter) {
		setter = mono_class_get_method_from_name (mono_defaults.object_class, "FieldSetter", -1);
		g_assert (setter);
	}

	if (field_class->valuetype)
		arg = mono_value_box (domain, field_class, val);
	else 
		arg = *((MonoObject **)val);
		

	msg = (MonoMethodMessage *)mono_object_new (domain, mono_defaults.mono_method_message_class);
	mono_message_init (domain, msg, mono_method_get_object (domain, setter, NULL), NULL);

	mono_array_set (msg->args, gpointer, 0, mono_string_new (domain, klass->name));
	mono_array_set (msg->args, gpointer, 1, mono_string_new (domain, field->name));
	mono_array_set (msg->args, gpointer, 2, arg);

	mono_remoting_invoke ((MonoObject *)(tp->rp), msg, &exc, &out_args);

	if (exc) mono_raise_exception ((MonoException *)exc);
}

/**
 * mono_store_remote_field_new:
 * @this:
 * @klass:
 * @field:
 * @arg:
 *
 * Missing documentation
 */
void
mono_store_remote_field_new (MonoObject *this, MonoClass *klass, MonoClassField *field, MonoObject *arg)
{
	static MonoMethod *setter = NULL;
	MonoDomain *domain = mono_domain_get ();
	MonoTransparentProxy *tp = (MonoTransparentProxy *) this;
	MonoClass *field_class;
	MonoMethodMessage *msg;
	MonoArray *out_args;
	MonoObject *exc;

	g_assert (this->vtable->klass == mono_defaults.transparent_proxy_class);

	field_class = mono_class_from_mono_type (field->type);

	if (tp->remote_class->proxy_class->contextbound && tp->rp->context == (MonoObject *) mono_context_get ()) {
		if (field_class->valuetype) mono_field_set_value (tp->rp->unwrapped_server, field, ((gchar *) arg) + sizeof (MonoObject));
		else mono_field_set_value (tp->rp->unwrapped_server, field, arg);
		return;
	}

	if (!setter) {
		setter = mono_class_get_method_from_name (mono_defaults.object_class, "FieldSetter", -1);
		g_assert (setter);
	}

	msg = (MonoMethodMessage *)mono_object_new (domain, mono_defaults.mono_method_message_class);
	mono_message_init (domain, msg, mono_method_get_object (domain, setter, NULL), NULL);

	mono_array_set (msg->args, gpointer, 0, mono_string_new (domain, klass->name));
	mono_array_set (msg->args, gpointer, 1, mono_string_new (domain, field->name));
	mono_array_set (msg->args, gpointer, 2, arg);

	mono_remoting_invoke ((MonoObject *)(tp->rp), msg, &exc, &out_args);

	if (exc) mono_raise_exception ((MonoException *)exc);
}

