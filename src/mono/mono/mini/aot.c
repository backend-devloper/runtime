/*
 * aot.c: mono Ahead of Time compiler
 *
 * Author:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#include "config.h"
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#ifndef PLATFORM_WIN32
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#include <limits.h>    /* for PAGESIZE */
#ifndef PAGESIZE
#define PAGESIZE 4096
#endif

#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/assembly.h>
#include <mono/os/gc_wrapper.h>

#include "mini.h"

#define ENCODE_TYPE_POS(t,l) (((t) << 24) | (l))
#define DECODE_TYPE(v) ((v) >> 24)
#define DECODE_POS(v) ((v) & 0xffffff)

#ifdef PLATFORM_WIN32
#define SHARED_EXT ".dll"
#else
#define SHARED_EXT ".so"
#endif

typedef struct MonoAotMethodInfo {
	MonoJitInfo *info;
	MonoJumpInfo *patch_info;
} MonoAotMethodInfo;

typedef struct MonoAotModuleInfo {
	/* Optimization flags used to compile the module */
	guint32 opts;
	/* Maps MonoMethods to MonoAotMethodInfos */
	MonoGHashTable *methods;
} MonoAotModuleInfo;

static MonoGHashTable *aot_modules;

static CRITICAL_SECTION aot_mutex;

static MonoClass * 
decode_class_info (gpointer *data)
{
	MonoImage *image;
	MonoClass *klass;
	
	image = mono_image_loaded_by_guid ((char *)data [1]);
	g_assert (image);

	if (data [0]) {
		return mono_class_get (image, (guint32)data [0]);
	} else {
		klass = decode_class_info (data [3]);
		return mono_array_class_get (klass, (guint32)data [2]);
	}

	return NULL;
}

static void
load_aot_module (MonoAssembly *assembly, gpointer user_data)
{
	char *aot_name;
	MonoAotModuleInfo *info;
	gboolean usable = TRUE;
	char *saved_guid = NULL;
	char *aot_version = NULL;
	char *opt_flags = NULL;

	aot_name = g_strdup_printf ("%s.so", assembly->image->name);

	assembly->aot_module = g_module_open (aot_name, G_MODULE_BIND_LAZY);

	if (!assembly->aot_module)
		return;

	g_module_symbol (assembly->aot_module, "mono_assembly_guid", (gpointer *) &saved_guid);
	g_module_symbol (assembly->aot_module, "mono_aot_version", (gpointer *) &aot_version);
	g_module_symbol (assembly->aot_module, "mono_aot_opt_flags", (gpointer *)&opt_flags);

	if (!aot_version || strcmp (aot_version, MONO_AOT_FILE_VERSION)) {
		printf ("AOT module %s has wrong file format version (expected %s got %s)\n", aot_name, MONO_AOT_FILE_VERSION, aot_version);
		usable = FALSE;
	}
	else
		if (!saved_guid || strcmp (assembly->image->guid, saved_guid)) {
			printf ("AOT module %s has a different GUID than the corresponding assembly.\n", aot_name);
			usable = FALSE;
		}
	g_free (aot_name);

	if (!usable) {
		g_module_close (assembly->aot_module);
		assembly->aot_module = NULL;
		return;
	}

	/*
	 * It seems that MonoGHashTables are in the GC heap, so structures
	 * containing them must be in the GC heap as well :(
	 */
#ifdef HAVE_BOEHM_GC
	info = GC_MALLOC (sizeof (MonoAotModuleInfo));
#else
	info = g_new0 (MonoAotModuleInfo, 1);
#endif
	info->methods = mono_g_hash_table_new (NULL, NULL);
	sscanf (opt_flags, "%d", &info->opts);

	EnterCriticalSection (&aot_mutex);
	mono_g_hash_table_insert (aot_modules, assembly, info);
	LeaveCriticalSection (&aot_mutex);

	printf ("Loaded AOT Module for %s.\n", assembly->image->name);
}

void
mono_aot_init (void)
{
	InitializeCriticalSection (&aot_mutex);

	aot_modules = mono_g_hash_table_new (NULL, NULL);

	mono_install_assembly_load_hook (load_aot_module, NULL);
}
 
static gpointer
mono_aot_get_method_inner (MonoDomain *domain, MonoMethod *method)
{
	MonoClass *klass = method->klass;
	MonoAssembly *ass = klass->image->assembly;
	MonoJumpInfo *patch_info = NULL;
	GModule *module = ass->aot_module;
	char *method_label, *info_label;
	guint8 *code = NULL;
	gpointer *info;
	guint code_len, used_int_regs, used_strings;
	MonoAotModuleInfo *aot_module;
	MonoAotMethodInfo *minfo;
	int i;

	if (!module)
		return NULL;

	if (!method->token)
		return NULL;

	aot_module = (MonoAotModuleInfo*)mono_g_hash_table_lookup (aot_modules, ass);

	g_assert (klass->inited);

	minfo = mono_g_hash_table_lookup (aot_module->methods, method);
	if (minfo) {
		MonoJitInfo *jinfo;

		jinfo = mono_mempool_alloc0 (domain->mp, sizeof (MonoJitInfo));
		memcpy (jinfo, minfo->info, sizeof (MonoJitInfo));

		/* This method was already loaded in another appdomain */
		if (aot_module->opts & MONO_OPT_SHARED)
			/* Use the same method in the new appdomain */
			;
		else {
			/* Create a copy of the original method and apply relocations */

			code = mono_mempool_alloc (domain->code_mp, minfo->info->code_size);
			memcpy (code, minfo->info->code_start, minfo->info->code_size);

			//printf ("REUSE METHOD: %s %p - %p.\n", mono_method_full_name (method, TRUE), code, (char*)code + code_len);

			/* Do this outside the lock to avoid deadlocks */
			LeaveCriticalSection (&aot_mutex);
			mono_arch_patch_code (method, domain, code, minfo->patch_info);
			EnterCriticalSection (&aot_mutex);

			jinfo->code_start = code;
		}

		mono_jit_info_table_add (domain, jinfo);
		return jinfo->code_start;
	}

	method_label = g_strdup_printf ("method_%08X", method->token);

	if (!g_module_symbol (module, method_label, (gpointer *)&code)) {
		g_free (method_label);		
		return NULL;
	}

	info_label = g_strdup_printf ("%s_patch_info", method_label);
	if (!g_module_symbol (module, info_label, (gpointer *)&info)) {
		g_free (method_label);		
		g_free (info_label);
		return NULL;
	}

	{
		static int count = 0;

		count ++;

		if (getenv ("MONO_LASTAOT")) {
			if (count > atoi(getenv ("MONO_LASTAOT"))) {
				return NULL;
			}
			else
				if (count == atoi(getenv ("MONO_LASTAOT")))
					printf ("LAST AOT METHOD: %s.%s.%s.\n", klass->name_space, klass->name, method->name);
		}
	}

	code_len = GPOINTER_TO_UINT (*((gpointer **)info));
	info++;
	used_int_regs = GPOINTER_TO_UINT (*((gpointer **)info));
	info++;
	used_strings = GPOINTER_TO_UINT (*((gpointer **)info));
	info++;

	//printf ("FOUND AOT compiled code for %s %p - %p %p\n", mono_method_full_name (method, TRUE), code, code + code_len, info);

	for (i = 0; i < used_strings; i++) {
		guint token =  GPOINTER_TO_UINT (*((gpointer **)info));
		info++;
		mono_ldstr (mono_root_domain, klass->image, mono_metadata_token_index (token));
	}

#ifdef HAVE_BOEHM_GC
	minfo = GC_MALLOC (sizeof (MonoAotMethodInfo));
#else
	minfo = g_new0 (MonoAotMethodInfo, 1);
#endif

	if (info) {
		MonoMemPool *mp = mono_mempool_new (); 
		MonoImage *image;
		guint8 *page_start;
		gpointer *table;
		int pages;
		int i, err;

		while (*info) {
			MonoJumpInfo *ji = mono_mempool_alloc0 (mp, sizeof (MonoJumpInfo));
			gpointer *data = *((gpointer **)info);
			info++;
			ji->type = DECODE_TYPE (GPOINTER_TO_UINT (*info));
			ji->ip.i = DECODE_POS (GPOINTER_TO_UINT (*info));

			switch (ji->type) {
			case MONO_PATCH_INFO_CLASS:
				ji->data.klass = decode_class_info (data);
				g_assert (ji->data.klass);
				mono_class_init (ji->data.klass);
				break;
			case MONO_PATCH_INFO_VTABLE:
				ji->data.klass = decode_class_info (data);
				g_assert (ji->data.klass);
				mono_class_init (ji->data.klass);
				break;
			case MONO_PATCH_INFO_IMAGE:
				ji->data.image = mono_image_loaded_by_guid ((char *)data);
				g_assert (ji->data.image);
				break;
			case MONO_PATCH_INFO_METHOD:
			case MONO_PATCH_INFO_METHODCONST:
				image = mono_image_loaded_by_guid ((char *)data [1]);
				g_assert (image);
				ji->data.method = mono_get_method (image, (guint32)data [0], NULL);
				g_assert (ji->data.method);
				mono_class_init (ji->data.method->klass);
				break;
			case MONO_PATCH_INFO_FIELD:
			case MONO_PATCH_INFO_SFLDA:
				image = mono_image_loaded_by_guid ((char *)data [1]);
				g_assert (image);
				ji->data.field = mono_field_from_token (image, (guint32)data [0], NULL);
				mono_class_init (ji->data.field->parent);
				g_assert (ji->data.field);
				break;
			case MONO_PATCH_INFO_INTERNAL_METHOD:
				ji->data.name = (char *)data;
				g_assert (ji->data.name);
				break;
			case MONO_PATCH_INFO_SWITCH:
				ji->table_size = (int)data [0];
				table = g_new (gpointer, ji->table_size);
				ji->data.target = table;
				for (i = 0; i < ji->table_size; i++) {
					table [i] = data [i + 1];
				}
				break;
			case MONO_PATCH_INFO_R4:
			case MONO_PATCH_INFO_R8:
				ji->data.target = data;
				break;
			case MONO_PATCH_INFO_LDSTR:
			case MONO_PATCH_INFO_LDTOKEN:
			case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
				ji->data.target = *data;
				break;
			case MONO_PATCH_INFO_EXC_NAME:
				ji->data.klass = decode_class_info (data);
				g_assert (ji->data.klass);
				mono_class_init (ji->data.klass);
				ji->data.name = ji->data.klass->name;
				break;
			case MONO_PATCH_INFO_METHOD_REL:
				ji->data.offset = (int)data [0];
				break;
			default:
				g_warning ("unhandled type %d", ji->type);
				g_assert_not_reached ();
			}

			info++;
			ji->next = patch_info;
			patch_info = ji;
		}

#ifndef PLATFORM_WIN32
		/* disable write protection */
		page_start = (char *) (((int) (code)) & ~ (PAGESIZE - 1));
		pages = (code + code_len - page_start + PAGESIZE - 1) / PAGESIZE;
		err = mprotect (page_start, pages * PAGESIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
		g_assert (err == 0);
#else
		{
			DWORD oldp;
			g_assert (VirtualProtect (code, code_len, PAGE_EXECUTE_READWRITE, &oldp) != 0);
		}
#endif

		/* Do this outside the lock to avoid deadlocks */
		LeaveCriticalSection (&aot_mutex);
		mono_arch_patch_code (method, domain, code, patch_info);
		EnterCriticalSection (&aot_mutex);

		if (aot_module->opts & MONO_OPT_SHARED)
			/* No need to cache this */
			mono_mempool_destroy (mp);
		else
			minfo->patch_info = patch_info;
	}

	g_free (info_label);
	g_free (method_label);

	{
		MonoJitInfo *info;
		info = mono_mempool_alloc0 (domain->mp, sizeof (MonoJitInfo));
		info->code_size = code_len;
		info->used_regs = used_int_regs;
		info->method = method;
		info->code_start = code;
		info->domain_neutral = (aot_module->opts & MONO_OPT_SHARED) != 0;
		mono_jit_info_table_add (domain, info);

		minfo->info = info;
		mono_g_hash_table_insert (aot_module->methods, method, minfo);
	}
	mono_jit_stats.methods_aot++;

	return code;
}

gpointer
mono_aot_get_method (MonoDomain *domain, MonoMethod *method)
{
	gpointer res;

	EnterCriticalSection (&aot_mutex);
	res = mono_aot_get_method_inner (domain, method);
	LeaveCriticalSection (&aot_mutex);
	return res;
}

#if 0
static void
write_data_symbol (FILE *fp, const char *name, guint8 *buf, int size, int align)
{
	int i;

	fprintf (fp, ".globl %s\n", name);
	fprintf (fp, ".data\n\t.align %d\n", align);
	fprintf (fp, "\t.type %s,@object\n", name);
	fprintf (fp, "\t.size %s,%d\n", name, size);
	fprintf (fp, "%s:\n", name);
	for (i = 0; i < size; i++) { 
		fprintf (fp, ".byte %d\n", buf [i]);
	}
	
}
#endif

static void
write_string_symbol (FILE *fp, const char *name, const char *value)
{
	fprintf (fp, ".globl %s\n", name);
	fprintf (fp, ".data\n");
	fprintf (fp, "%s:\n", name);
	fprintf (fp, "\t.string \"%s\"\n", value);
}

static guint32
mono_get_field_token (MonoClassField *field) 
{
	MonoClass *klass = field->parent;
	int i;

	for (i = 0; i < klass->field.count; ++i) {
		if (field == &klass->fields [i])
			return MONO_TOKEN_FIELD_DEF | (klass->field.first + 1 + i);
	}

	g_assert_not_reached ();
	return 0;
}

static char *
cond_emit_image_label (FILE *tmpfp, GHashTable *image_hash, MonoImage *image)
{
	char *label;
	
	if ((label = g_hash_table_lookup (image_hash, image))) 
		return label;

	label = g_strdup_printf ("image_patch_info_%p", image);
	fprintf (tmpfp, "%s:\n", label);
	fprintf (tmpfp, "\t.string \"%s\"\n", image->guid);

	g_hash_table_insert (image_hash, image, label);

	return label;
}

static char *
cond_emit_icall_label (FILE *tmpfp, GHashTable *hash, const char *icall_name)
{
	char *label;

	if ((label = g_hash_table_lookup (hash, icall_name))) 
		return label;

	label = g_strdup_printf ("icall_patch_info_%s", icall_name);
	fprintf (tmpfp, "%s:\n", label);
	fprintf (tmpfp, "\t.string \"%s\"\n", icall_name);

	g_hash_table_insert (hash, (gpointer)icall_name, label);

	return label;
}

static char *
cond_emit_method_label (FILE *tmpfp, GHashTable *hash, MonoJumpInfo *patch_info)
{
	MonoMethod *method = patch_info->data.method;
	char *l1, *l2;

	if ((l1 = g_hash_table_lookup (hash, method))) 
		return l1;
	
	l2 = cond_emit_image_label (tmpfp, hash, method->klass->image);
	fprintf (tmpfp, "\t.align %d\n", sizeof (gpointer));
	l1 = g_strdup_printf ("method_patch_info_%08x_%p", method->token, method);
	fprintf (tmpfp, "%s:\n", l1);
	fprintf (tmpfp, "\t.long 0x%08x\n", method->token);
	g_assert (method->token);
	fprintf (tmpfp, "\t.long %s\n", l2);
		
	g_hash_table_insert (hash, method, l1);

	return l1;
}

static char *
cond_emit_klass_label (FILE *tmpfp, GHashTable *hash, MonoClass *klass)
{
	char *l1, *l2, *el = NULL;

	if ((l1 = g_hash_table_lookup (hash, klass))) 
		return l1;

	if (!klass->type_token) {
		g_assert (klass->rank > 0);
		el = cond_emit_klass_label (tmpfp, hash, klass->element_class);
	}
	
	l2 = cond_emit_image_label (tmpfp, hash, klass->image);
	fprintf (tmpfp, "\t.align %d\n", sizeof (gpointer));
	l1 = g_strdup_printf ("klass_patch_info_%08x_%p", klass->type_token, klass);
	fprintf (tmpfp, "%s:\n", l1);
	fprintf (tmpfp, "\t.long 0x%08x\n", klass->type_token);
	fprintf (tmpfp, "\t.long %s\n", l2);

	if (el) {
		fprintf (tmpfp, "\t.long %d\n", klass->rank);	
		fprintf (tmpfp, "\t.long %s\n", el);
	}

	g_hash_table_insert (hash, klass, l1);

	return l1;
}

static char *
cond_emit_field_label (FILE *tmpfp, GHashTable *hash, MonoJumpInfo *patch_info)
{
	MonoClassField *field = patch_info->data.field;
	char *l1, *l2;
	guint token;

	if ((l1 = g_hash_table_lookup (hash, field))) 
		return l1;
	
	l2 = cond_emit_image_label (tmpfp, hash, field->parent->image);
	fprintf (tmpfp, "\t.align %d\n", sizeof (gpointer));
	token = mono_get_field_token (field);
	l1 = g_strdup_printf ("klass_patch_info_%08x_%p", token, field);
	fprintf (tmpfp, "%s:\n", l1);
	fprintf (tmpfp, "\t.long 0x%08x\n", token);
	g_assert (token);
	fprintf (tmpfp, "\t.long %s\n", l2);
		
	g_hash_table_insert (hash, field, l1);

	return l1;
}

int
mono_compile_assembly (MonoAssembly *ass, guint32 opts)
{
	MonoCompile *cfg;
	MonoImage *image = ass->image;
	MonoMethod *method;
	GList *l;
	char *com, *tmpfname, *opts_str;
	FILE *tmpfp;
	int i, j;
	guint8 *code, *mname;
	int ccount = 0, mcount = 0, lmfcount = 0, ecount = 0, abscount = 0, wrappercount = 0, ocount = 0;
	GHashTable *ref_hash;
	int func_alignment = 16;

	printf ("Mono AOT compiler - compiling assembly %s\n", image->name);

	i = g_file_open_tmp ("mono_aot_XXXXXX", &tmpfname, NULL);
	tmpfp = fdopen (i, "w+");
	g_assert (tmpfp);

	ref_hash = g_hash_table_new (NULL, NULL);

	write_string_symbol (tmpfp, "mono_assembly_guid" , image->guid);

	write_string_symbol (tmpfp, "mono_aot_version", MONO_AOT_FILE_VERSION);

	opts_str = g_strdup_printf ("%d", opts);
	write_string_symbol (tmpfp, "mono_aot_opt_flags", opts_str);
	g_free (opts_str);

	for (i = 0; i < image->tables [MONO_TABLE_METHOD].rows; ++i) {
		MonoJumpInfo *patch_info;
		gboolean skip;
		guint32 token = MONO_TOKEN_METHOD_DEF | (i + 1);
       	        method = mono_get_method (image, token, NULL);
		
		/* fixme: maybe we can also precompile wrapper methods */
		if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) ||
		    (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
		    (method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) ||
		    (method->flags & METHOD_ATTRIBUTE_ABSTRACT)) {
			//printf ("Skip (impossible): %s\n", mono_method_full_name (method, TRUE));
			continue;
		}

		mcount++;

		/* fixme: we need to patch the IP for the LMF in that case */
		if (method->save_lmf) {
			//printf ("Skip (needs lmf):  %s\n", mono_method_full_name (method, TRUE));
			lmfcount++;
			continue;
		}

		/* fixme: add methods with exception tables */
		if (((MonoMethodNormal *)method)->header->num_clauses) {
			//printf ("Skip (exceptions): %s\n", mono_method_full_name (method, TRUE));
			ecount++;
			continue;
		}

		//printf ("START:           %s\n", mono_method_full_name (method, TRUE));
		//mono_compile_method (method);

		cfg = mini_method_compile (method, opts, mono_root_domain, 0);
		g_assert (cfg);

		if (cfg->disable_aot) {
			ocount++;
			continue;
		}

		skip = FALSE;
		for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
			if (patch_info->type == MONO_PATCH_INFO_ABS) {
				/* unable to handle this */
				//printf ("Skip (abs addr):   %s %d\n", mono_method_full_name (method, TRUE), patch_info->type);
				skip = TRUE;	
				break;
			}
		}

		if (skip) {
			abscount++;
			continue;
		}

		skip = FALSE;
		for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
			if ((patch_info->type == MONO_PATCH_INFO_METHOD ||
			     patch_info->type == MONO_PATCH_INFO_METHODCONST) &&
			    patch_info->data.method->wrapper_type) {
				/* unable to handle this */
				//printf ("Skip (wrapper call):   %s %d\n", mono_method_full_name (method, TRUE), patch_info->type);
				skip = TRUE;	
				break;
			}
		}

		if (skip) {
			wrappercount++;
			continue;
		}

		//printf ("Compile:           %s\n", mono_method_full_name (method, TRUE));

		code = cfg->native_code;

		fprintf (tmpfp, ".text\n");
		mname = g_strdup_printf ("method_%08X", token);
		fprintf (tmpfp, "\t.align %d\n", func_alignment);
		fprintf (tmpfp, ".globl %s\n", mname);
		fprintf (tmpfp, "\t.type %s,@function\n", mname);
		fprintf (tmpfp, "%s:\n", mname);

		for (j = 0; j < cfg->code_len; j++) 
			fprintf (tmpfp, ".byte %d\n", (unsigned int) code [j]);

		fprintf (tmpfp, ".data\n");

		j = 0;
		for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
			switch (patch_info->type) {
			case MONO_PATCH_INFO_LABEL:
			case MONO_PATCH_INFO_BB:
				/* relative jumps are no problem, there is no need to handle then here */
				break;
			case MONO_PATCH_INFO_SWITCH: {
				gpointer *table = (gpointer *)patch_info->data.target;
				int k;

				fprintf (tmpfp, "\t.align %d\n", sizeof (gpointer));
				fprintf (tmpfp, "%s_patch_info_%d:\n", mname, j);
				fprintf (tmpfp, "\t.long %d\n", patch_info->table_size);

				for (k = 0; k < patch_info->table_size; k++) {
					fprintf (tmpfp, "\t.long %d\n", (int)table [k]);
				}
				j++;
				break;
			}
			case MONO_PATCH_INFO_INTERNAL_METHOD:
				patch_info->data.name = cond_emit_icall_label (tmpfp, ref_hash, patch_info->data.name);
				j++;
				break;
			case MONO_PATCH_INFO_METHODCONST:
			case MONO_PATCH_INFO_METHOD:
				patch_info->data.name = cond_emit_method_label (tmpfp, ref_hash, patch_info);
				j++;
				break;
			case MONO_PATCH_INFO_FIELD:
				patch_info->data.name = cond_emit_field_label (tmpfp, ref_hash, patch_info);
				j++;
				break;
			case MONO_PATCH_INFO_CLASS:
				patch_info->data.name = cond_emit_klass_label (tmpfp, ref_hash, patch_info->data.klass);
				j++;
				break;
			case MONO_PATCH_INFO_IMAGE:
				patch_info->data.name = cond_emit_image_label (tmpfp, ref_hash, patch_info->data.image);
				j++;
				break;
			case MONO_PATCH_INFO_EXC_NAME: {
				MonoClass *ex_class;

				ex_class =
					mono_class_from_name (mono_defaults.exception_class->image,
										  "System", patch_info->data.target);
				g_assert (ex_class);
				patch_info->data.name = cond_emit_klass_label (tmpfp, ref_hash, ex_class);
				j++;
				break;
			}
			case MONO_PATCH_INFO_R4:
				fprintf (tmpfp, "\t.align 8\n");
				fprintf (tmpfp, "%s_patch_info_%d:\n", mname, j);
				fprintf (tmpfp, "\t.long 0x%08x\n", *((guint32 *)patch_info->data.target));
				
				j++;
				break;
			case MONO_PATCH_INFO_R8:
				fprintf (tmpfp, "\t.align 8\n");
				fprintf (tmpfp, "%s_patch_info_%d:\n", mname, j);
				fprintf (tmpfp, "\t.long 0x%08x\n", *((guint32 *)patch_info->data.target));
				fprintf (tmpfp, "\t.long 0x%08x\n", *((guint32 *)patch_info->data.target + 1));
				
				j++;
				break;
			case MONO_PATCH_INFO_METHOD_REL:
				fprintf (tmpfp, "\t.align %d\n", sizeof (gpointer));
				fprintf (tmpfp, "%s_patch_info_%d:\n", mname, j);
				fprintf (tmpfp, "\t.long 0x%08x\n", patch_info->data.offset);
				j++;
				break;
			case MONO_PATCH_INFO_VTABLE:
				patch_info->data.name = cond_emit_klass_label (tmpfp, ref_hash, patch_info->data.klass);
				j++;
				break;
			case MONO_PATCH_INFO_SFLDA:
				patch_info->data.name = cond_emit_field_label (tmpfp, ref_hash, patch_info);
				j++;
				break;
			case MONO_PATCH_INFO_LDSTR:
			case MONO_PATCH_INFO_LDTOKEN:
			case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
				fprintf (tmpfp, "\t.align 8\n");
				fprintf (tmpfp, "%s_patch_info_%d:\n", mname, j);
				fprintf (tmpfp, "\t.long 0x%08x\n", patch_info->data.token);
				j++;
				break;
			default:
				g_warning ("unable to handle jump info %d", patch_info->type);
				g_assert_not_reached ();
			}
		}
		
		fprintf (tmpfp, ".globl %s_patch_info\n", mname);
		fprintf (tmpfp, "\t.align %d\n", sizeof (gpointer));
		fprintf (tmpfp, "%s_patch_info:\n", mname);
		
		fprintf (tmpfp, "\t.long %d\n", cfg->code_len);
		fprintf (tmpfp, "\t.long %d\n", cfg->used_int_regs);

		fprintf (tmpfp, "\t.long %d\n", g_list_length (cfg->ldstr_list));
		for (l = cfg->ldstr_list; l; l = l->next) {
			fprintf (tmpfp, "\t.long 0x%08lx\n", (long)l->data);
		}

		if (j) {
			j = 0;
			for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
				switch (patch_info->type) {
				case MONO_PATCH_INFO_METHODCONST:
				case MONO_PATCH_INFO_METHOD:
				case MONO_PATCH_INFO_CLASS:
				case MONO_PATCH_INFO_FIELD:
				case MONO_PATCH_INFO_INTERNAL_METHOD:
				case MONO_PATCH_INFO_IMAGE:
				case MONO_PATCH_INFO_VTABLE:
				case MONO_PATCH_INFO_SFLDA:
				case MONO_PATCH_INFO_EXC_NAME:
					fprintf (tmpfp, "\t.long %s\n", patch_info->data.name);
					fprintf (tmpfp, "\t.long 0x%08x\n", ENCODE_TYPE_POS (patch_info->type, patch_info->ip.i));
					j++;
					break;
				case MONO_PATCH_INFO_SWITCH:
				case MONO_PATCH_INFO_R4:
				case MONO_PATCH_INFO_R8:
				case MONO_PATCH_INFO_METHOD_REL:
				case MONO_PATCH_INFO_LDSTR:
				case MONO_PATCH_INFO_LDTOKEN:
				case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
					fprintf (tmpfp, "\t.long %s_patch_info_%d\n", mname, j);
					fprintf (tmpfp, "\t.long 0x%08x\n", ENCODE_TYPE_POS (patch_info->type, patch_info->ip.i));
					j++;
					break;
				case MONO_PATCH_INFO_LABEL:
				case MONO_PATCH_INFO_BB:
					break;
				default:
					g_warning ("unable to handle jump info %d", patch_info->type);
					g_assert_not_reached ();
				}

			}
		}
		/* NULL terminated array */
		fprintf (tmpfp, "\t.long 0\n");

		/* fixme: save the rest of the required infos */

		g_free (mname);
		mono_destroy_compile (cfg);

		ccount++;
	}

	fclose (tmpfp);

	com = g_strdup_printf ("as %s -o %s.o", tmpfname, tmpfname);
	printf ("Executing the native assembler: %s\n", com);
	system (com);
	g_free (com);
	com = g_strdup_printf ("ld -shared -o %s%s %s.o", image->name, SHARED_EXT, tmpfname);
	printf ("Executing the native linker: %s\n", com);
	system (com);
	g_free (com);
	com = g_strdup_printf ("%s.o", tmpfname);
	unlink (com);
	g_free (com);
	/*com = g_strdup_printf ("strip --strip-unneeded %s%s", image->name, SHARED_EXT);
	printf ("Stripping the binary: %s\n", com);
	system (com);
	g_free (com);*/

	printf ("Compiled %d out of %d methods (%d%%)\n", ccount, mcount, (ccount*100)/mcount);
	printf ("%d methods contain exception tables (%d%%)\n", ecount, (ecount*100)/mcount);
	printf ("%d methods contain absolute addresses (%d%%)\n", abscount, (abscount*100)/mcount);
	printf ("%d methods contain wrapper references (%d%%)\n", wrappercount, (wrappercount*100)/mcount);
	printf ("%d methods contain lmf pointers (%d%%)\n", lmfcount, (lmfcount*100)/mcount);
	printf ("%d methods have other problems (%d%%)\n", ocount, (ocount*100)/mcount);
	unlink (tmpfname);

	return 0;
}
