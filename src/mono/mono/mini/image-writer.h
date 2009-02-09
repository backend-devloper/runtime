/*
 * image-writer.h: Creation of object files or assembly files using the same interface.
 *
 * Author:
 *   Dietmar Maurer (dietmar@ximian.com) MONO_INTERNAL;
 *   Zoltan Varga (vargaz@gmail.com) MONO_INTERNAL;
 *   Paolo Molaro (lupus@ximian.com) MONO_INTERNAL;
 *
 * (C) MONO_INTERNAL; 2002 Ximian, Inc.
 */

#ifndef __MONO_IMAGE_WRITER_H__
#define __MONO_IMAGE_WRITER_H__

#include "config.h"

#include <glib.h>
#include <stdio.h>

#include <mono/utils/mono-compiler.h>

typedef struct _MonoImageWriter MonoImageWriter;

gboolean bin_writer_supported (void) MONO_INTERNAL;

MonoImageWriter* img_writer_create (FILE *fp, gboolean use_bin_writer) MONO_INTERNAL;

void img_writer_destroy (MonoImageWriter *w) MONO_INTERNAL;

void img_writer_emit_start (MonoImageWriter *w) MONO_INTERNAL;

int img_writer_emit_writeout (MonoImageWriter *w) MONO_INTERNAL;

void img_writer_emit_section_change (MonoImageWriter *w, const char *section_name, int subsection_index) MONO_INTERNAL;

void img_writer_emit_push_section (MonoImageWriter *w, const char *section_name, int subsection) MONO_INTERNAL;

void img_writer_emit_pop_section (MonoImageWriter *w) MONO_INTERNAL;

void img_writer_emit_global (MonoImageWriter *w, const char *name, gboolean func) MONO_INTERNAL;

void img_writer_emit_local_symbol (MonoImageWriter *w, const char *name, const char *end_label, gboolean func) MONO_INTERNAL;

void img_writer_emit_label (MonoImageWriter *w, const char *name) MONO_INTERNAL;

void img_writer_emit_bytes (MonoImageWriter *w, const guint8* buf, int size) MONO_INTERNAL;

void img_writer_emit_string (MonoImageWriter *w, const char *value) MONO_INTERNAL;

void img_writer_emit_line (MonoImageWriter *w) MONO_INTERNAL;

void img_writer_emit_alignment (MonoImageWriter *w, int size) MONO_INTERNAL;

void img_writer_emit_pointer_unaligned (MonoImageWriter *w, const char *target) MONO_INTERNAL;

void img_writer_emit_pointer (MonoImageWriter *w, const char *target) MONO_INTERNAL;

void img_writer_emit_int16 (MonoImageWriter *w, int value) MONO_INTERNAL;

void img_writer_emit_int32 (MonoImageWriter *w, int value) MONO_INTERNAL;

void img_writer_emit_symbol_diff (MonoImageWriter *w, const char *end, const char* start, int offset) MONO_INTERNAL;

void img_writer_emit_zero_bytes (MonoImageWriter *w, int num) MONO_INTERNAL;

void img_writer_emit_global (MonoImageWriter *w, const char *name, gboolean func) MONO_INTERNAL;

void img_writer_emit_byte (MonoImageWriter *w, guint8 val) MONO_INTERNAL;

void img_writer_emit_reloc (MonoImageWriter *acfg, int reloc_type, const char *symbol, int addend) MONO_INTERNAL;

#endif
