/*
 * Copyright 2014 Xamarin Inc
 */
 
#ifndef __MONO_SEQ_POINTS_H__
#define __MONO_SEQ_POINTS_H__

#define MONO_SEQ_POINT_FLAG_NONEMPTY_STACK 1

/* IL offsets used to mark the sequence points belonging to method entry/exit events */
#define METHOD_ENTRY_IL_OFFSET -1
#define METHOD_EXIT_IL_OFFSET 0xffffff

/* Native offset used to mark seq points in dead code */
#define SEQ_POINT_NATIVE_OFFSET_DEAD_CODE -1

typedef struct {
	int il_offset, native_offset, flags;
	/* Indexes of successor sequence points */
	int *next;
	/* Number of entries in next */
	int next_len;
} SeqPoint;

typedef struct MonoSeqPointInfo {
	int len;
	SeqPoint seq_points [MONO_ZERO_LEN_ARRAY];
} MonoSeqPointInfo;

void
mono_save_seq_point_info (MonoCompile *cfg);

MonoSeqPointInfo*
get_seq_points (MonoDomain *domain, MonoMethod *method);

SeqPoint*
find_next_seq_point_for_native_offset (MonoDomain *domain, MonoMethod *method, gint32 native_offset, MonoSeqPointInfo **info);

SeqPoint*
find_prev_seq_point_for_native_offset (MonoDomain *domain, MonoMethod *method, gint32 native_offset, MonoSeqPointInfo **info);

G_GNUC_UNUSED SeqPoint*
find_seq_point (MonoDomain *domain, MonoMethod *method, gint32 il_offset, MonoSeqPointInfo **info);

#endif /* __MONO_SEQ_POINTS_H__ */