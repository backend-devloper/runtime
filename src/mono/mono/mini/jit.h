/*
 * Author:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001, 2002, 2003 Ximian, Inc.
 */

#ifndef _MONO_JIT_JIT_H_
#define _MONO_JIT_JIT_H_

#include <mono/metadata/appdomain.h>

G_BEGIN_DECLS

MonoDomain * 
mono_jit_init              (const char *file);

MonoDomain * 
mono_jit_init_version      (const char *root_domain_name, const char *runtime_version);

int
mono_jit_exec              (MonoDomain *domain, MonoAssembly *assembly, 
			    int argc, char *argv[]);
void        
mono_jit_cleanup           (MonoDomain *domain);

gboolean
mono_jit_set_trace_options (const char* options);

void
mono_set_signal_chaining   (gboolean chain_signals);

void
mono_jit_set_aot_only      (gboolean aot_only);

void
mono_jit_parse_options     (int argc, char * argv[]);

G_END_DECLS

#endif

