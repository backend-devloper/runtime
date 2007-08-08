/*
 * driver.c: The new mono JIT compiler.
 *
 * Author:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2002-2003 Ximian, Inc.
 * (C) 2003-2006 Novell, Inc.
 */

#include <config.h>
#include <signal.h>
#if HAVE_SCHED_SETAFFINITY
#include <sched.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <mono/metadata/assembly.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/cil-coff.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/socket-io.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/io-layer/io-layer.h>
#include "mono/metadata/profiler.h"
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/verify.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/security-manager.h>
#include <mono/os/gc_wrapper.h>
#include "mono/utils/mono-counters.h"

#include "mini.h"
#include "jit.h"
#include <string.h>
#include <ctype.h>
#include "inssel.h"
#include <locale.h>
#include "version.h"

static FILE *mini_stats_fd = NULL;

static void mini_usage (void);

/* This turns off command line globbing under win32 */
#ifdef PLATFORM_WIN32
int _CRT_glob = 0;
#endif

typedef void (*OptFunc) (const char *p);

#undef OPTFLAG
#ifdef HAVE_ARRAY_ELEM_INIT
#define MSGSTRFIELD(line) MSGSTRFIELD1(line)
#define MSGSTRFIELD1(line) str##line

static const struct msgstr_t {
#define OPTFLAG(id,shift,name,desc) char MSGSTRFIELD(__LINE__) [sizeof (name) + sizeof (desc)];
#include "optflags-def.h"
#undef OPTFLAG
} opstr = {
#define OPTFLAG(id,shift,name,desc) name "\0" desc,
#include "optflags-def.h"
#undef OPTFLAG
};
static const gint16 opt_names [] = {
#define OPTFLAG(id,shift,name,desc) [(shift)] = offsetof (struct msgstr_t, MSGSTRFIELD(__LINE__)),
#include "optflags-def.h"
#undef OPTFLAG
};

#define optflag_get_name(id) ((const char*)&opstr + opt_names [(id)])
#define optflag_get_desc(id) (optflag_get_name(id) + 1 + strlen (optflag_get_name(id)))

#else /* !HAVE_ARRAY_ELEM_INIT */
typedef struct {
	const char* name;
	const char* desc;
} OptName;

#define OPTFLAG(id,shift,name,desc) {name,desc},
static const OptName 
opt_names [] = {
#include "optflags-def.h"
	{NULL, NULL}
};
#define optflag_get_name(id) (opt_names [(id)].name)
#define optflag_get_desc(id) (opt_names [(id)].desc)

#endif

static const OptFunc
opt_funcs [sizeof (int) * 8] = {
	NULL
};

#define DEFAULT_OPTIMIZATIONS (	\
	MONO_OPT_PEEPHOLE |	\
	MONO_OPT_CFOLD |	\
	MONO_OPT_INLINE |       \
	MONO_OPT_CONSPROP |     \
	MONO_OPT_COPYPROP |     \
	MONO_OPT_TREEPROP |     \
	MONO_OPT_DEADCE |       \
	MONO_OPT_BRANCH |	\
	MONO_OPT_LINEARS |	\
	MONO_OPT_INTRINS |  \
	MONO_OPT_LOOP |  \
	MONO_OPT_EXCEPTION |  \
	MONO_OPT_AOT)

#define EXCLUDED_FROM_ALL (MONO_OPT_SHARED | MONO_OPT_PRECOMP)

static guint32
parse_optimizations (const char* p)
{
	/* the default value */
	guint32 opt = DEFAULT_OPTIMIZATIONS;
	guint32 exclude = 0;
	const char *n;
	int i, invert, len;

	/* call out to cpu detection code here that sets the defaults ... */
	opt |= mono_arch_cpu_optimizazions (&exclude);
	opt &= ~exclude;
	if (!p)
		return opt;

	while (*p) {
		if (*p == '-') {
			p++;
			invert = TRUE;
		} else {
			invert = FALSE;
		}
		for (i = 0; i < G_N_ELEMENTS (opt_names) && optflag_get_name (i); ++i) {
			n = optflag_get_name (i);
			len = strlen (n);
			if (strncmp (p, n, len) == 0) {
				if (invert)
					opt &= ~ (1 << i);
				else
					opt |= 1 << i;
				p += len;
				if (*p == ',') {
					p++;
					break;
				} else if (*p == '=') {
					p++;
					if (opt_funcs [i])
						opt_funcs [i] (p);
					while (*p && *p++ != ',');
					break;
				}
				/* error out */
				break;
			}
		}
		if (i == G_N_ELEMENTS (opt_names) || !optflag_get_name (i)) {
			if (strncmp (p, "all", 3) == 0) {
				if (invert)
					opt = 0;
				else
					opt = ~(EXCLUDED_FROM_ALL | exclude);
				p += 3;
				if (*p == ',')
					p++;
			} else {
				fprintf (stderr, "Invalid optimization name `%s'\n", p);
				exit (1);
			}
		}
	}
	return opt;
}

typedef struct {
	const char name [6];
	const char desc [18];
	MonoGraphOptions value;
} GraphName;

static const GraphName 
graph_names [] = {
	{"cfg",      "Control Flow",                            MONO_GRAPH_CFG},
	{"dtree",    "Dominator Tree",                          MONO_GRAPH_DTREE},
	{"code",     "CFG showing code",                        MONO_GRAPH_CFG_CODE},
	{"ssa",      "CFG after SSA",                           MONO_GRAPH_CFG_SSA},
	{"optc",     "CFG after IR opts",                       MONO_GRAPH_CFG_OPTCODE}
};

static MonoGraphOptions
mono_parse_graph_options (const char* p)
{
	const char *n;
	int i, len;

	for (i = 0; i < G_N_ELEMENTS (graph_names); ++i) {
		n = graph_names [i].name;
		len = strlen (n);
		if (strncmp (p, n, len) == 0)
			return graph_names [i].value;
	}

	fprintf (stderr, "Invalid graph name provided: %s\n", p);
	exit (1);
}

int
mono_parse_default_optimizations (const char* p)
{
	guint32 opt;

	opt = parse_optimizations (p);
	return opt;
}

static char*
opt_descr (guint32 flags) {
	GString *str = g_string_new ("");
	int i, need_comma;

	need_comma = 0;
	for (i = 0; i < G_N_ELEMENTS (opt_names); ++i) {
		if (flags & (1 << i)) {
			if (need_comma)
				g_string_append_c (str, ',');
			g_string_append (str, optflag_get_name (i));
			need_comma = 1;
		}
	}
	return g_string_free (str, FALSE);
}

static const guint32
opt_sets [] = {
       0,
       MONO_OPT_PEEPHOLE,
       MONO_OPT_BRANCH,
       MONO_OPT_CFOLD,
       MONO_OPT_FCMOV,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_INTRINS,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_CFOLD,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_SSA,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_EXCEPTION,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_EXCEPTION | MONO_OPT_ABCREM,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_EXCEPTION | MONO_OPT_ABCREM | MONO_OPT_SSAPRE,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_ABCREM,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_TREEPROP,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_SSAPRE,
       MONO_OPT_BRANCH | MONO_OPT_PEEPHOLE | MONO_OPT_LINEARS | MONO_OPT_COPYPROP | MONO_OPT_CONSPROP | MONO_OPT_DEADCE | MONO_OPT_LOOP | MONO_OPT_INLINE | MONO_OPT_INTRINS | MONO_OPT_ABCREM | MONO_OPT_SHARED
};

typedef int (*TestMethod) (void);

#if 0
static void
domain_dump_native_code (MonoDomain *domain) {
	// need to poke into the domain, move to metadata/domain.c
	// need to empty jit_info_table and code_mp
}
#endif

static int
mini_regression (MonoImage *image, int verbose, int *total_run) {
	guint32 i, opt, opt_flags;
	MonoMethod *method;
	MonoCompile *cfg;
	char *n;
	int result, expected, failed, cfailed, run, code_size, total;
	TestMethod func;
	GTimer *timer = g_timer_new ();

	if (mini_stats_fd) {
		fprintf (mini_stats_fd, "$stattitle = \'Mono Benchmark Results (various optimizations)\';\n");

		fprintf (mini_stats_fd, "$graph->set_legend(qw(");
		for (opt = 0; opt < G_N_ELEMENTS (opt_sets); opt++) {
			opt_flags = opt_sets [opt];
			n = opt_descr (opt_flags);
			if (!n [0])
				n = (char *)"none";
			if (opt)
				fprintf (mini_stats_fd, " ");
			fprintf (mini_stats_fd, "%s", n);
		

		}
		fprintf (mini_stats_fd, "));\n");

		fprintf (mini_stats_fd, "@data = (\n");
		fprintf (mini_stats_fd, "[");
	}

	/* load the metadata */
	for (i = 0; i < mono_image_get_table_rows (image, MONO_TABLE_METHOD); ++i) {
       	        method = mono_get_method (image, MONO_TOKEN_METHOD_DEF | (i + 1), NULL);
		mono_class_init (method->klass);

		if (!strncmp (method->name, "test_", 5) && mini_stats_fd) {
			fprintf (mini_stats_fd, "\"%s\",", method->name);
		}
	}
	if (mini_stats_fd)
		fprintf (mini_stats_fd, "],\n");


	total = 0;
	*total_run = 0;
	for (opt = 0; opt < G_N_ELEMENTS (opt_sets); ++opt) {
		double elapsed, comp_time, start_time;

		opt_flags = opt_sets [opt];
		mono_set_defaults (verbose, opt_flags);
		n = opt_descr (opt_flags);
		g_print ("Test run: image=%s, opts=%s\n", mono_image_get_filename (image), n);
		g_free (n);
		cfailed = failed = run = code_size = 0;
		comp_time = elapsed = 0.0;

		/* fixme: ugly hack - delete all previously compiled methods */
		g_hash_table_destroy (mono_domain_get ()->jit_trampoline_hash);
		mono_domain_get ()->jit_trampoline_hash = g_hash_table_new (mono_aligned_addr_hash, NULL);
		mono_internal_hash_table_destroy (&(mono_domain_get ()->jit_code_hash));
		mono_jit_code_hash_init (&(mono_domain_get ()->jit_code_hash));

		g_timer_start (timer);
		if (mini_stats_fd)
			fprintf (mini_stats_fd, "[");
		for (i = 0; i < mono_image_get_table_rows (image, MONO_TABLE_METHOD); ++i) {
        	        method = mono_get_method (image, MONO_TOKEN_METHOD_DEF | (i + 1), NULL);
			if (strncmp (method->name, "test_", 5) == 0) {
				expected = atoi (method->name + 5);
				run++;
				start_time = g_timer_elapsed (timer, NULL);
				comp_time -= start_time; 
				cfg = mini_method_compile (method, opt_flags, mono_get_root_domain (), TRUE, FALSE, 0);
				comp_time += g_timer_elapsed (timer, NULL);
				if (cfg->exception_type == MONO_EXCEPTION_NONE) {
					if (verbose >= 2)
						g_print ("Running '%s' ...\n", method->name);
#ifdef MONO_USE_AOT_COMPILER
					if ((func = mono_aot_get_method (mono_get_root_domain (), method)))
						;
					else
#endif
						func = (TestMethod)(gpointer)cfg->native_code;
					func = (TestMethod)mono_create_ftnptr (mono_get_root_domain (), func);
					result = func ();
					if (result != expected) {
						failed++;
						g_print ("Test '%s' failed result (got %d, expected %d).\n", method->name, result, expected);
					}
					code_size += cfg->code_len;
					mono_destroy_compile (cfg);

				} else {
					cfailed++;
					if (verbose)
						g_print ("Test '%s' failed compilation.\n", method->name);
				}
				if (mini_stats_fd)
					fprintf (mini_stats_fd, "%f, ", 
						 g_timer_elapsed (timer, NULL) - start_time);
			}
		}
		if (mini_stats_fd)
			fprintf (mini_stats_fd, "],\n");
		g_timer_stop (timer);
		elapsed = g_timer_elapsed (timer, NULL);
		if (failed > 0 || cfailed > 0){
			g_print ("Results: total tests: %d, failed: %d, cfailed: %d (pass: %.2f%%)\n", 
				 run, failed, cfailed, 100.0*(run-failed-cfailed)/run);
		} else {
			g_print ("Results: total tests: %d, all pass \n",  run);
		}
		
		g_print ("Elapsed time: %f secs (%f, %f), Code size: %d\n\n", elapsed, 
			 elapsed - comp_time, comp_time, code_size);
		total += failed + cfailed;
		*total_run += run;
	}

	if (mini_stats_fd) {
		fprintf (mini_stats_fd, ");\n");
		fflush (mini_stats_fd);
	}

	g_timer_destroy (timer);
	return total;
}

static int
mini_regression_list (int verbose, int count, char *images [])
{
	int i, total, total_run, run;
	MonoAssembly *ass;
	
	total_run =  total = 0;
	for (i = 0; i < count; ++i) {
		ass = mono_assembly_open (images [i], NULL);
		if (!ass) {
			g_warning ("failed to load assembly: %s", images [i]);
			continue;
		}
		total += mini_regression (mono_assembly_get_image (ass), verbose, &run);
		total_run += run;
	}
	if (total > 0){
		g_print ("Overall results: tests: %d, failed: %d, opt combinations: %d (pass: %.2f%%)\n", 
			 total_run, total, (int)G_N_ELEMENTS (opt_sets), 100.0*(total_run-total)/total_run);
	} else {
		g_print ("Overall results: tests: %d, 100%% pass, opt combinations: %d\n", 
			 total_run, (int)G_N_ELEMENTS (opt_sets));
	}
	
	return total;
}

enum {
	DO_BENCH,
	DO_REGRESSION,
	DO_COMPILE,
	DO_EXEC,
	DO_DRAW,
	DO_DEBUGGER
};

typedef struct CompileAllThreadArgs {
	MonoAssembly *ass;
	int verbose;
	guint32 opts;
} CompileAllThreadArgs;

static void
compile_all_methods_thread_main (CompileAllThreadArgs *args)
{
	MonoAssembly *ass = args->ass;
	int verbose = args->verbose;
	MonoImage *image = mono_assembly_get_image (ass);
	MonoMethod *method;
	MonoCompile *cfg;
	int i, count = 0;

	for (i = 0; i < mono_image_get_table_rows (image, MONO_TABLE_METHOD); ++i) {
		guint32 token = MONO_TOKEN_METHOD_DEF | (i + 1);
		MonoMethodSignature *sig;

		if (mono_metadata_has_generic_params (image, token))
			continue;

		method = mono_get_method (image, token, NULL);
		if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) ||
		    (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
		    (method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) ||
		    (method->flags & METHOD_ATTRIBUTE_ABSTRACT))
			continue;

		if (method->klass->generic_container)
			continue;
		sig = mono_method_signature (method);
		if (sig->has_type_parameters)
			continue;

		count++;
		if (verbose) {
			char * desc = mono_method_full_name (method, TRUE);
			g_print ("Compiling %d %s\n", count, desc);
			g_free (desc);
		}
		cfg = mini_method_compile (method, args->opts, mono_get_root_domain (), FALSE, FALSE, 0);
		mono_destroy_compile (cfg);
	}

}

static void
compile_all_methods (MonoAssembly *ass, int verbose, guint32 opts)
{
	CompileAllThreadArgs args;

	args.ass = ass;
	args.verbose = verbose;
	args.opts = opts;

	/* 
	 * Need to create a mono thread since compilation might trigger
	 * running of managed code.
	 */
	mono_thread_create (mono_domain_get (), compile_all_methods_thread_main, &args);

	mono_thread_manage ();
}

/**
 * mono_jit_exec:
 * @assembly: reference to an assembly
 * @argc: argument count
 * @argv: argument vector
 *
 * Start execution of a program.
 */
int 
mono_jit_exec (MonoDomain *domain, MonoAssembly *assembly, int argc, char *argv[])
{
	MonoImage *image = mono_assembly_get_image (assembly);
	MonoMethod *method;
	guint32 entry = mono_image_get_entry_point (image);

	if (!entry) {
		g_print ("Assembly '%s' doesn't have an entry point.\n", mono_image_get_filename (image));
		/* FIXME: remove this silly requirement. */
		mono_environment_exitcode_set (1);
		return 1;
	}

	method = mono_get_method (image, entry, NULL);
	if (method == NULL){
		g_print ("The entry point method could not be loaded\n");
		mono_environment_exitcode_set (1);
		return 1;
	}
	
	return mono_runtime_run_main (method, argc, argv, NULL);
}

typedef struct 
{
	MonoDomain *domain;
	const char *file;
	int argc;
	char **argv;
	guint32 opts;
	char *aot_options;
} MainThreadArgs;

static void main_thread_handler (gpointer user_data)
{
	MainThreadArgs *main_args = user_data;
	MonoAssembly *assembly;

	assembly = mono_domain_assembly_open (main_args->domain, main_args->file);
	if (!assembly){
		fprintf (stderr, "Can not open image %s\n", main_args->file);
		exit (1);
	}

	if (mono_compile_aot) {
		int res = mono_compile_assembly (assembly, main_args->opts, main_args->aot_options);
		printf ("AOT RESULT %d\n", res);
	} else {
		/* 
		 * This must be done in a thread managed by mono since it can invoke
		 * managed code.
		 */
		if (main_args->opts & MONO_OPT_PRECOMP)
			mono_precompile_assemblies ();

		mono_jit_exec (main_args->domain, assembly, main_args->argc, main_args->argv);
	}
}

static void
mini_usage_jitdeveloper (void)
{
	int i;
	
	fprintf (stdout,
		 "Runtime and JIT debugging options:\n"
		 "    --breakonex            Inserts a breakpoint on exceptions\n"
		 "    --break METHOD         Inserts a breakpoint at METHOD entry\n"
		 "    --compile METHOD       Just compile METHOD in assembly\n"
		 "    --compile-all          Compiles all the methods in the assembly\n"
		 "    --ncompile N           Number of times to compile METHOD (default: 1)\n"
		 "    --print-vtable         Print the vtable of all used classes\n"
		 "    --regression           Runs the regression test contained in the assembly\n"
		 "    --statfile FILE        Sets the stat file to FILE\n"
		 "    --stats                Print statistics about the JIT operations\n"
		 "    --wapi=hps|semdel      IO-layer maintenance\n"
		 "\n"
		 "Other options:\n" 
		 "    --graph[=TYPE] METHOD  Draws a graph of the specified method:\n");
	
	for (i = 0; i < G_N_ELEMENTS (graph_names); ++i) {
		fprintf (stdout, "                           %-10s %s\n", graph_names [i].name, graph_names [i].desc);
	}
}

static void
mini_usage_list_opt (void)
{
	int i;
	
	for (i = 0; i < G_N_ELEMENTS (opt_names); ++i)
		fprintf (stdout, "                           %-10s %s\n", optflag_get_name (i), optflag_get_desc (i));
}

static void
mini_usage (void)
{
	fprintf (stdout,
		"Usage is: mono [options] program [program-options]\n"
		"\n"
		"Development:\n"
		"    --aot                  Compiles the assembly to native code\n"
		"    --debug                Enable debugging support\n"
		"    --profile[=profiler]   Runs in profiling mode with the specified profiler module\n"
		"    --trace[=EXPR]         Enable tracing, use --help-trace for details\n"
		"    --help-devel           Shows more options available to developers\n"
		"\n"
		"Runtime:\n"
		"    --config FILE          Loads FILE as the Mono config\n"
		"    --verbose, -v          Increases the verbosity level\n"
		"    --help, -h             Show usage information\n"
		"    --version, -V          Show version information\n"
		"    --runtime=VERSION      Use the VERSION runtime, instead of autodetecting\n"
		"    --optimize=OPT         Turns on or off a specific optimization\n"
		"                           Use --list-opt to get a list of optimizations\n"
		"    --security[=mode]      Turns on the unsupported security manager (off by default)\n"
		"                           mode is one of cas or core-clr\n");
}

static void
mini_trace_usage (void)
{
	fprintf (stdout,
		 "Tracing options:\n"
		 "   --trace[=EXPR]        Trace every call, optional EXPR controls the scope\n"
		 "\n"
		 "EXPR is composed of:\n"
		 "    all                  All assemblies\n"
		 "    none                 No assemblies\n"
		 "    program              Entry point assembly\n"
		 "    assembly             Specifies an assembly\n"
		 "    M:Type:Method        Specifies a method\n"
		 "    N:Namespace          Specifies a namespace\n"
		 "    T:Type               Specifies a type\n"
		 "    +EXPR                Includes expression\n"
		 "    -EXPR                Excludes expression\n"
		 "    disabled             Don't print any output until toggled via SIGUSR2\n");
}

static const char info[] =
#ifdef HAVE_KW_THREAD
	"\tTLS:           __thread\n"
#else
	"\tTLS:           normal\n"
#endif /* HAVE_KW_THREAD */
	"\tGC:            " USED_GC_NAME "\n"
#ifdef MONO_ARCH_SIGSEGV_ON_ALTSTACK
    "\tSIGSEGV:       altstack\n"
#else
    "\tSIGSEGV:       normal\n"
#endif
#ifdef HAVE_EPOLL
    "\tNotifications: epoll\n"
#else
    "\tNotification:  Thread + polling\n"
#endif
        "\tArchitecture:  " ARCHITECTURE "\n"
	"\tDisabled:      " DISABLED_FEATURES "\n"
	"";

#ifndef MONO_ARCH_AOT_SUPPORTED
#define error_if_aot_unsupported() do {fprintf (stderr, "AOT compilation is not supported on this platform.\n"); exit (1);} while (0)
#else
#define error_if_aot_unsupported()
#endif

int
mono_main (int argc, char* argv[])
{
	MainThreadArgs main_args;
	MonoAssembly *assembly;
	MonoMethodDesc *desc;
	MonoMethod *method;
	MonoCompile *cfg;
	MonoDomain *domain;
	const char* aname, *mname = NULL;
	char *config_file = NULL;
	int i, count = 1;
	int enable_debugging = FALSE;
	guint32 opt, action = DO_EXEC;
	MonoGraphOptions mono_graph_options = 0;
	int mini_verbose = 0;
	gboolean enable_profile = FALSE;
	char *trace_options = NULL;
	char *profile_options = NULL;
	char *aot_options = NULL;
	char *forced_version = NULL;

	setlocale (LC_ALL, "");

#if HAVE_SCHED_SETAFFINITY
	if (getenv ("MONO_NO_SMP")) {
		unsigned long proc_mask = 1;
		sched_setaffinity (getpid(), sizeof (unsigned long), &proc_mask);
	}
#endif
	if (!g_thread_supported ())
		g_thread_init (NULL);

	if (mono_running_on_valgrind () && getenv ("MONO_VALGRIND_LEAK_CHECK")) {
		GMemVTable mem_vtable;

		/* 
		 * Instruct glib to use the system allocation functions so valgrind
		 * can track the memory allocated by the g_... functions.
		 */
		memset (&mem_vtable, 0, sizeof (mem_vtable));
		mem_vtable.malloc = malloc;
		mem_vtable.realloc = realloc;
		mem_vtable.free = free;
		mem_vtable.calloc = calloc;

		g_mem_set_vtable (&mem_vtable);
	}

	g_log_set_always_fatal (G_LOG_LEVEL_ERROR);
	g_log_set_fatal_mask (G_LOG_DOMAIN, G_LOG_LEVEL_ERROR);

	opt = parse_optimizations (NULL);

	for (i = 1; i < argc; ++i) {
		if (argv [i] [0] != '-')
			break;
		if (strcmp (argv [i], "--regression") == 0) {
			action = DO_REGRESSION;
		} else if (strcmp (argv [i], "--verbose") == 0 || strcmp (argv [i], "-v") == 0) {
			mini_verbose++;
		} else if (strcmp (argv [i], "--version") == 0 || strcmp (argv [i], "-V") == 0) {
			g_print ("Mono JIT compiler version %s (%s)\nCopyright (C) 2002-2007 Novell, Inc and Contributors. www.mono-project.com\n", VERSION, FULL_VERSION);
			g_print (info);
			if (mini_verbose) {
				const char *cerror;
				const char *clibpath;
				mono_init ("mono");
				cerror = mono_check_corlib_version ();
				clibpath = mono_defaults.corlib? mono_image_get_filename (mono_defaults.corlib): "unknown";
				if (cerror) {
					g_print ("The currently installed mscorlib doesn't match this runtime version.\n");
					g_print ("The error is: %s\n", cerror);
					g_print ("mscorlib.dll loaded at: %s\n", clibpath);
					return 1;
				}
			}
			return 0;
		} else if (strcmp (argv [i], "--help") == 0 || strcmp (argv [i], "-h") == 0) {
			mini_usage ();
			return 0;
		} else if (strcmp (argv [i], "--help-trace") == 0){
			mini_trace_usage ();
			return 0;
		} else if (strcmp (argv [i], "--help-devel") == 0){
			mini_usage_jitdeveloper ();
			return 0;
		} else if (strcmp (argv [i], "--list-opt") == 0){
			mini_usage_list_opt ();
			return 0;
		} else if (strncmp (argv [i], "--statfile", 10) == 0) {
			if (i + 1 >= argc){
				fprintf (stderr, "error: --statfile requires a filename argument\n");
				return 1;
			}
			mini_stats_fd = fopen (argv [++i], "w+");
		} else if (strncmp (argv [i], "--optimize=", 11) == 0) {
			opt = parse_optimizations (argv [i] + 11);
		} else if (strncmp (argv [i], "-O=", 3) == 0) {
			opt = parse_optimizations (argv [i] + 3);
		} else if (strcmp (argv [i], "--config") == 0) {
			if (i +1 >= argc){
				fprintf (stderr, "error: --config requires a filename argument\n");
				return 1;
			}
			config_file = argv [++i];
		} else if (strcmp (argv [i], "--ncompile") == 0) {
			if (i + 1 >= argc){
				fprintf (stderr, "error: --ncompile requires an argument\n");
				return 1;
			}
			count = atoi (argv [++i]);
			action = DO_BENCH;
		} else if (strcmp (argv [i], "--trace") == 0) {
			trace_options = (char*)"";
		} else if (strncmp (argv [i], "--trace=", 8) == 0) {
			trace_options = &argv [i][8];
		} else if (strcmp (argv [i], "--breakonex") == 0) {
			mono_break_on_exc = TRUE;
		} else if (strcmp (argv [i], "--break") == 0) {
			if (i+1 >= argc){
				fprintf (stderr, "Missing method name in --break command line option\n");
				return 1;
			}
			
			if (!mono_debugger_insert_breakpoint (argv [++i], FALSE))
				fprintf (stderr, "Error: invalid method name '%s'\n", argv [i]);
		} else if (strcmp (argv [i], "--print-vtable") == 0) {
			mono_print_vtable = TRUE;
		} else if (strcmp (argv [i], "--stats") == 0) {
			mono_counters_enable (-1);
			mono_stats.enabled = TRUE;
			mono_jit_stats.enabled = TRUE;
#ifndef DISABLE_AOT
		} else if (strcmp (argv [i], "--aot") == 0) {
			error_if_aot_unsupported ();
			mono_compile_aot = TRUE;
		} else if (strncmp (argv [i], "--aot=", 6) == 0) {
			error_if_aot_unsupported ();
			mono_compile_aot = TRUE;
			aot_options = &argv [i][6];
#endif
		} else if (strcmp (argv [i], "--compile-all") == 0) {
			action = DO_COMPILE;
		} else if (strncmp (argv [i], "--runtime=", 10) == 0) {
			forced_version = &argv [i][10];
		} else if (strcmp (argv [i], "--profile") == 0) {
			enable_profile = TRUE;
			profile_options = NULL;
		} else if (strncmp (argv [i], "--profile=", 10) == 0) {
			enable_profile = TRUE;
			profile_options = argv [i] + 10;
		} else if (strcmp (argv [i], "--compile") == 0) {
			if (i + 1 >= argc){
				fprintf (stderr, "error: --compile option requires a method name argument\n");
				return 1;
			}
			
			mname = argv [++i];
			action = DO_BENCH;
		} else if (strncmp (argv [i], "--graph=", 8) == 0) {
			if (i + 1 >= argc){
				fprintf (stderr, "error: --graph option requires a method name argument\n");
				return 1;
			}
			
			mono_graph_options = mono_parse_graph_options (argv [i] + 8);
			mname = argv [++i];
			action = DO_DRAW;
		} else if (strcmp (argv [i], "--graph") == 0) {
			if (i + 1 >= argc){
				fprintf (stderr, "error: --graph option requires a method name argument\n");
				return 1;
			}
			
			mname = argv [++i];
			mono_graph_options = MONO_GRAPH_CFG;
			action = DO_DRAW;
		} else if (strcmp (argv [i], "--debug") == 0) {
			enable_debugging = TRUE;
		} else if (strcmp (argv [i], "--security") == 0) {
			mono_security_mode = MONO_SECURITY_MODE_CAS;
			mono_activate_security_manager ();
		} else if (strncmp (argv [i], "--security=", 11) == 0) {
			if (strcmp (argv [i] + 11, "temporary-smcs-hack") == 0) {
				mono_security_mode = MONO_SECURITY_MODE_SMCS_HACK;
			} else if (strcmp (argv [i] + 11, "core-clr") == 0) {
				mono_security_mode = MONO_SECURITY_MODE_CORE_CLR;
			} else if (strcmp (argv [i] + 11, "cas") == 0){
				mono_security_mode = MONO_SECURITY_MODE_CAS;
				mono_activate_security_manager ();
			} else {
				fprintf (stderr, "error: --security= option has invalid argument (cas or core-clr)\n");
				return 1;
			}
		} else if (strcmp (argv [i], "--desktop") == 0) {
#if defined (HAVE_BOEHM_GC)
			GC_dont_expand = 1;
#endif
			/* Put desktop-specific optimizations here */
		} else if (strcmp (argv [i], "--server") == 0){
			/* Put server-specific optimizations here */
		} else if (strcmp (argv [i], "--inside-mdb") == 0) {
			action = DO_DEBUGGER;
		} else if (strncmp (argv [i], "--wapi=", 7) == 0) {
			if (strcmp (argv [i] + 7, "hps") == 0) {
				return mini_wapi_hps (argc - i, argv + i);
			} else if (strcmp (argv [i] + 7, "semdel") == 0) {
				return mini_wapi_semdel (argc - i, argv + i);
			} else if (strcmp (argv [i] + 7, "seminfo") == 0) {
				return mini_wapi_seminfo (argc - i, argv + i);
			} else {
				fprintf (stderr, "Invalid --wapi suboption: '%s'\n", argv [i]);
				return 1;
			}
		} else {
			fprintf (stderr, "Unknown command line option: '%s'\n", argv [i]);
			return 1;
		}
	}

	if (!argv [i]) {
		mini_usage ();
		return 1;
	}

	if ((action == DO_EXEC) && g_getenv ("MONO_INSIDE_MDB"))
		action = DO_DEBUGGER;

	if (mono_compile_aot || action == DO_EXEC || action == DO_DEBUGGER) {
		g_set_prgname (argv[i]);
	}

	if (enable_profile) {
		/* Needed because of TLS accesses in mono_profiler_load () */
		MONO_GC_PRE_INIT ();
		mono_profiler_load (profile_options);
	}

	if (trace_options != NULL){
		/* 
		 * Need to call this before mini_init () so we can trace methods 
		 * compiled there too.
		 */
		mono_jit_trace_calls = mono_trace_parse_options (trace_options);
		if (mono_jit_trace_calls == NULL)
			exit (1);
	}

	if (action == DO_DEBUGGER) {
		// opt |= MONO_OPT_SHARED;
		opt &= ~MONO_OPT_INLINE;
		opt &= ~MONO_OPT_COPYPROP;
		opt &= ~MONO_OPT_CONSPROP;
		enable_debugging = TRUE;

#ifdef MONO_DEBUGGER_SUPPORTED
		mono_debug_init (MONO_DEBUG_FORMAT_DEBUGGER);
		mono_debugger_init ();
#else
		g_print ("The Mono Debugger is not supported on this platform.\n");
		return 1;
#endif
	}

	mono_set_defaults (mini_verbose, opt);
	domain = mini_init (argv [i], forced_version);
	
	switch (action) {
	case DO_REGRESSION:
		if (mini_regression_list (mini_verbose, argc -i, argv + i)) {
			g_print ("Regression ERRORS!\n");
			mini_cleanup (domain);
			return 1;
		}
		mini_cleanup (domain);
		return 0;
	case DO_BENCH:
		if (argc - i != 1 || mname == NULL) {
			g_print ("Usage: mini --ncompile num --compile method assembly\n");
			mini_cleanup (domain);
			return 1;
		}
		aname = argv [i];
		break;
	case DO_COMPILE:
		if (argc - i != 1) {
			mini_usage ();
			mini_cleanup (domain);
			return 1;
		}
		aname = argv [i];
		break;
	case DO_DRAW:
		if (argc - i != 1 || mname == NULL) {
			mini_usage ();
			mini_cleanup (domain);
			return 1;
		}
		aname = argv [i];
		break;
	default:
		if (argc - i < 1) {
			mini_usage ();
			mini_cleanup (domain);
			return 1;
		}
		aname = argv [i];
		break;
	}

	if (action == DO_DEBUGGER)
		mono_debug_init_1 (domain);
	else if (enable_debugging) {
		mono_debug_init (MONO_DEBUG_FORMAT_MONO);
		mono_debug_init_1 (domain);
	}

	/* Parse gac loading options before loading assemblies. */
	if (mono_compile_aot || action == DO_EXEC || action == DO_DEBUGGER) {
		mono_config_parse (config_file);
	}

	assembly = mono_assembly_open (aname, NULL);
	if (!assembly) {
		fprintf (stderr, "Cannot open assembly %s.\n", aname);
		mini_cleanup (domain);
		return 2;
	}

	if (trace_options != NULL)
		mono_trace_set_assembly (assembly);

	if (enable_debugging)
		mono_debug_init_2 (assembly);

	if (mono_compile_aot || action == DO_EXEC) {
		const char *error;

		//mono_set_rootdir ();

		error = mono_check_corlib_version ();
		if (error) {
			fprintf (stderr, "Corlib not in sync with this runtime: %s\n", error);
			fprintf (stderr, "Loaded from: %s\n",
				mono_defaults.corlib? mono_image_get_filename (mono_defaults.corlib): "unknown");
			fprintf (stderr, "Download a newer corlib or a newer runtime at http://www.go-mono.com/daily.\n");
			exit (1);
		}

#ifdef PLATFORM_WIN32
		/* Detach console when executing IMAGE_SUBSYSTEM_WINDOWS_GUI on win32 */
		if (!enable_debugging && !mono_compile_aot && ((MonoCLIImageInfo*)(mono_assembly_get_image (assembly)->image_info))->cli_header.nt.pe_subsys_required == IMAGE_SUBSYSTEM_WINDOWS_GUI)
			FreeConsole ();
#endif

		main_args.domain = domain;
		main_args.file = aname;		
		main_args.argc = argc - i;
		main_args.argv = argv + i;
		main_args.opts = opt;
		main_args.aot_options = aot_options;
#if RUN_IN_SUBTHREAD
		mono_runtime_exec_managed_code (domain, main_thread_handler, &main_args);
#else
		main_thread_handler (&main_args);
		mono_thread_manage ();
#endif
		mini_cleanup (domain);

		/* Look up return value from System.Environment.ExitCode */
		i = mono_environment_exitcode_get ();
		return i;
	} else if (action == DO_COMPILE) {
		compile_all_methods (assembly, mini_verbose, opt);
		mini_cleanup (domain);
		return 0;
	} else if (action == DO_DEBUGGER) {
#ifdef MONO_DEBUGGER_SUPPORTED
		const char *error;

		error = mono_check_corlib_version ();
		if (error) {
			fprintf (stderr, "Corlib not in sync with this runtime: %s\n", error);
			fprintf (stderr, "Download a newer corlib or a newer runtime at http://www.go-mono.com/daily.\n");
			exit (1);
		}

		mono_debugger_main (domain, assembly, argc - i, argv + i);
		mini_cleanup (domain);
		return 0;
#else
		return 1;
#endif
	}
	desc = mono_method_desc_new (mname, 0);
	if (!desc) {
		g_print ("Invalid method name %s\n", mname);
		mini_cleanup (domain);
		return 3;
	}
	method = mono_method_desc_search_in_image (desc, mono_assembly_get_image (assembly));
	if (!method) {
		g_print ("Cannot find method %s\n", mname);
		mini_cleanup (domain);
		return 3;
	}

	if (action == DO_DRAW) {
		int part = 0;

		switch (mono_graph_options) {
		case MONO_GRAPH_DTREE:
			part = 1;
			opt |= MONO_OPT_LOOP;
			break;
		case MONO_GRAPH_CFG_CODE:
			part = 1;
			break;
		case MONO_GRAPH_CFG_SSA:
			part = 2;
			break;
		case MONO_GRAPH_CFG_OPTCODE:
			part = 3;
			break;
		default:
			break;
		}

		if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) ||
			(method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)) {
			MonoMethod *nm;
			nm = mono_marshal_get_native_wrapper (method);
			cfg = mini_method_compile (nm, opt, mono_get_root_domain (), FALSE, FALSE, part);
		}
		else
			cfg = mini_method_compile (method, opt, mono_get_root_domain (), FALSE, FALSE, part);
		if ((mono_graph_options & MONO_GRAPH_CFG_SSA) && !(cfg->comp_done & MONO_COMP_SSA)) {
			g_warning ("no SSA info available (use -O=deadce)");
			return 1;
		}
		mono_draw_graph (cfg, mono_graph_options);
		mono_destroy_compile (cfg);

	} else if (action == DO_BENCH) {
		if (mini_stats_fd) {
			const char *n;
			double no_opt_time = 0.0;
			GTimer *timer = g_timer_new ();
			fprintf (mini_stats_fd, "$stattitle = \'Compilations times for %s\';\n", 
				 mono_method_full_name (method, TRUE));
			fprintf (mini_stats_fd, "@data = (\n");
			fprintf (mini_stats_fd, "[");
			for (i = 0; i < G_N_ELEMENTS (opt_sets); i++) {
				opt = opt_sets [i];
				n = opt_descr (opt);
				if (!n [0])
					n = "none";
				fprintf (mini_stats_fd, "\"%s\",", n);
			}
			fprintf (mini_stats_fd, "],\n[");

			for (i = 0; i < G_N_ELEMENTS (opt_sets); i++) {
				int j;
				double elapsed;
				opt = opt_sets [i];
				g_timer_start (timer);
				for (j = 0; j < count; ++j) {
					cfg = mini_method_compile (method, opt, mono_get_root_domain (), FALSE, FALSE, 0);
					mono_destroy_compile (cfg);
				}
				g_timer_stop (timer);
				elapsed = g_timer_elapsed (timer, NULL);
				if (!opt)
					no_opt_time = elapsed;
				fprintf (mini_stats_fd, "%f, ", elapsed);
			}
			fprintf (mini_stats_fd, "]");
			if (no_opt_time > 0.0) {
				fprintf (mini_stats_fd, ", \n[");
				for (i = 0; i < G_N_ELEMENTS (opt_sets); i++) 
					fprintf (mini_stats_fd, "%f,", no_opt_time);
				fprintf (mini_stats_fd, "]");
			}
			fprintf (mini_stats_fd, ");\n");
		} else {
			for (i = 0; i < count; ++i) {
				if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) ||
					(method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL))
					method = mono_marshal_get_native_wrapper (method);

				cfg = mini_method_compile (method, opt, mono_get_root_domain (), FALSE, FALSE, 0);
				mono_destroy_compile (cfg);
			}
		}
	} else {
		cfg = mini_method_compile (method, opt, mono_get_root_domain (), FALSE, FALSE, 0);
		mono_destroy_compile (cfg);
	}

	mini_cleanup (domain);
 	return 0;
}

MonoDomain * 
mono_jit_init (const char *file)
{
	return mini_init (file, NULL);
}

/**
 * mono_jit_init_version:
 * @file: the initial assembly to load
 * @runtime_version: the version of the runtime to load
 *
 * Use this version when you want to force a particular runtime
 * version to be used.  By default Mono will pick the runtime that is
 * referenced by the initial assembly (specified in @file), this
 * routine allows programmers to specify the actual runtime to be used
 * as the initial runtime is inherited by all future assemblies loaded
 * (since Mono does not support having more than one mscorlib runtime
 * loaded at once).
 *
 * The @runtime_version can be one of these strings: "v1.1.4322" for
 * the 1.1 runtime or "v2.0.50727"  for the 2.0 runtime. 
 *
 * Returns: the MonoDomain representing the domain where the assembly
 * was loaded.
 */
MonoDomain * 
mono_jit_init_version (const char *file, const char *runtime_version)
{
	return mini_init (file, runtime_version);
}

void        
mono_jit_cleanup (MonoDomain *domain)
{
	mini_cleanup (domain);
}
