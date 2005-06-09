/*
 * mono-gc.h: GC related public interface
 *
 */
#ifndef __METADATA_MONO_GC_H__
#define __METADATA_MONO_GC_H__

#include <glib.h>

G_BEGIN_DECLS

void   mono_gc_collect         (int generation);
int    mono_gc_max_generation  (void);
gint64 mono_gc_get_used_size   (void);
gint64 mono_gc_get_heap_size   (void);

G_END_DECLS

#endif /* __METADATA_MONO_GC_H__ */

