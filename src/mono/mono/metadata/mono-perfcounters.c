/*
 * mono-perfcounters.c
 *
 * Performance counters support.
 *
 * Author: Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2008 Novell, Inc
 */

#include "config.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include "metadata/mono-perfcounters.h"
#include "metadata/appdomain.h"
/* for mono_stats */
#include "metadata/class-internals.h"
#include "utils/mono-time.h"
#include "utils/mono-mmap.h"
#include <mono/io-layer/io-layer.h>

/* map of CounterSample.cs */
struct _MonoCounterSample {
	gint64 rawValue;
	gint64 baseValue;
	gint64 counterFrequency;
	gint64 systemFrequency;
	gint64 timeStamp;
	gint64 timeStamp100nSec;
	gint64 counterTimeStamp;
	int counterType;
};

/* map of PerformanceCounterType.cs */
enum {
	NumberOfItemsHEX32=0x00000000,
	NumberOfItemsHEX64=0x00000100,
	NumberOfItems32=0x00010000,
	NumberOfItems64=0x00010100,
	CounterDelta32=0x00400400,
	CounterDelta64=0x00400500,
	SampleCounter=0x00410400,
	CountPerTimeInterval32=0x00450400,
	CountPerTimeInterval64=0x00450500,
	RateOfCountsPerSecond32=0x10410400,
	RateOfCountsPerSecond64=0x10410500,
	RawFraction=0x20020400,
	CounterTimer=0x20410500,
	Timer100Ns=0x20510500,
	SampleFraction=0x20C20400,
	CounterTimerInverse=0x21410500,
	Timer100NsInverse=0x21510500,
	CounterMultiTimer=0x22410500,
	CounterMultiTimer100Ns=0x22510500,
	CounterMultiTimerInverse=0x23410500,
	CounterMultiTimer100NsInverse=0x23510500,
	AverageTimer32=0x30020400,
	ElapsedTime=0x30240500,
	AverageCount64=0x40020500,
	SampleBase=0x40030401,
	AverageBase=0x40030402,
	RawBase=0x40030403,
	CounterMultiBase=0x42030500
};

/* maps a small integer type to the counter types above */
static const int
simple_type_to_type [] = {
	NumberOfItemsHEX32, NumberOfItemsHEX64,
	NumberOfItems32, NumberOfItems64,
	CounterDelta32, CounterDelta64,
	SampleCounter, CountPerTimeInterval32,
	CountPerTimeInterval64, RateOfCountsPerSecond32,
	RateOfCountsPerSecond64, RawFraction,
	CounterTimer, Timer100Ns,
	SampleFraction, CounterTimerInverse,
	Timer100NsInverse, CounterMultiTimer,
	CounterMultiTimer100Ns, CounterMultiTimerInverse,
	CounterMultiTimer100NsInverse, AverageTimer32,
	ElapsedTime, AverageCount64,
	SampleBase, AverageBase,
	RawBase, CounterMultiBase
};

enum {
	SingleInstance,
	MultiInstance,
	CatTypeUnknown = -1
};

enum {
	ProcessInstance,
	ThreadInstance,
	CPUInstance,
	MonoInstance,
	CustomInstance
};

#define PERFCTR_CAT(id,name,help,type,inst,first_counter) CATEGORY_ ## id,
#define PERFCTR_COUNTER(id,name,help,type)
enum {
#include "mono-perfcounters-def.h"
	NUM_CATEGORIES
};

#undef PERFCTR_CAT
#undef PERFCTR_COUNTER
#define PERFCTR_CAT(id,name,help,type,inst,first_counter) CATEGORY_START_ ## id = -1,
#define PERFCTR_COUNTER(id,name,help,type) COUNTER_ ## id,
/* each counter is assigned an id starting from 0 inside the category */
enum {
#include "mono-perfcounters-def.h"
	END_COUNTERS
};

#undef PERFCTR_CAT
#undef PERFCTR_COUNTER
#define PERFCTR_CAT(id,name,help,type,inst,first_counter)
#define PERFCTR_COUNTER(id,name,help,type) CCOUNTER_ ## id,
/* this is used just to count the number of counters */
enum {
#include "mono-perfcounters-def.h"
	NUM_COUNTERS
};

static CRITICAL_SECTION perfctr_mutex;
#define perfctr_lock() EnterCriticalSection (&perfctr_mutex)
#define perfctr_unlock() LeaveCriticalSection (&perfctr_mutex)

typedef struct {
	char reserved [16];
	int size;
	unsigned short counters_start;
	unsigned short counters_size;
	unsigned short data_start;
	MonoPerfCounters counters;
	char data [1];
} MonoSharedArea;

/*
  binary format of custom counters in shared memory, starting from MonoSharedArea* + data_start;
  basic stanza:
  struct stanza_header {
  	byte stanza_type; // FTYPE_*
  	byte other_info;
  	ushort stanza_length; // includeas header
  	... data ...
  }

// strings are utf8
// perfcat and perfinstance are 4-bytes aligned
struct perfcat {
	byte typeidx;
	byte categorytype;
	ushort length; // includes the counters
	ushort num_counters;
	ushort counters_data_size;
	int num_instances;
	char name[]; // null terminated
	char help[]; // null terminated
	// perfcounters follow
	{
		byte countertype;
		char name[]; // null terminated
		char help[]; // null terminated
	}
	0-byte
};

struct perfinstance {
	byte typeidx;
	byte data_offset; // offset of counters from beginning of struct
	ushort length;
	uint category_offset; // offset of category in the shared area
	char name[]; // null terminated
	// data follows: this is always 8-byte aligned
};

*/

enum {
	FTYPE_CATEGORY = 'C',
	FTYPE_DELETED = 'D',
	FTYPE_PREDEF_INSTANCE = 'P', // an instance of a predef counter
	FTYPE_INSTANCE = 'I',
	FTYPE_DIRTY = 'd',
	FTYPE_END = 0
};

typedef struct {
	unsigned char ftype;
	unsigned char extra;
	unsigned short size;
} SharedHeader;

typedef struct {
	SharedHeader header;
	unsigned short num_counters;
	unsigned short counters_data_size;
	int num_instances;
	/* variable length data follows */
	char name [1];
} SharedCategory;

typedef struct {
	SharedHeader header;
	unsigned int category_offset;
	/* variable length data follows */
	char name [1];
} SharedInstance;

typedef struct {
	unsigned char type;
	/* variable length data follows */
	char name [1];
} SharedCounter;

typedef struct {
	const char *name;
	const char *help;
	unsigned char id;
	signed int type : 2;
	unsigned int instance_type : 6;
	short first_counter;
} CategoryDesc;

typedef struct {
	const char *name;
	const char *help;
	int id;
	int type;
} CounterDesc;

#undef PERFCTR_CAT
#undef PERFCTR_COUNTER
#define PERFCTR_CAT(id,name,help,type,inst,first_counter) {name, help, CATEGORY_ ## id, type, inst ## Instance, CCOUNTER_ ## first_counter},
#define PERFCTR_COUNTER(id,name,help,type)
static const CategoryDesc
predef_categories [] = {
#include "mono-perfcounters-def.h"
	{NULL, NULL, NUM_CATEGORIES, -1, 0, NUM_COUNTERS}
};

#undef PERFCTR_CAT
#undef PERFCTR_COUNTER
#define PERFCTR_CAT(id,name,help,type,inst,first_counter)
#define PERFCTR_COUNTER(id,name,help,type) {name, help, COUNTER_ ## id, type},
static const CounterDesc
predef_counters [] = {
#include "mono-perfcounters-def.h"
	{NULL, NULL, -1, 0}
};

/*
 * We have several different classes of counters:
 * *) system counters
 * *) runtime counters
 * *) remote counters
 * *) user-defined counters
 * *) windows counters (the implementation on windows will use this)
 *
 * To easily handle the differences we create a vtable for each class that contains the
 * function pointers with the actual implementation to access the counters.
 */
typedef struct _ImplVtable ImplVtable;

typedef MonoBoolean (*SampleFunc) (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample* sample);
typedef gint64 (*UpdateFunc) (ImplVtable *vtable, MonoBoolean do_incr, gint64 value);
typedef void (*CleanupFunc) (ImplVtable *vtable);

struct _ImplVtable {
	void *arg;
	SampleFunc sample;
	UpdateFunc update;
	CleanupFunc cleanup;
};

typedef struct {
	ImplVtable vtable;
	MonoPerfCounters *counters;
	int pid;
} PredefVtable;

static ImplVtable*
create_vtable (void *arg, SampleFunc sample, UpdateFunc update)
{
	ImplVtable *vtable = g_new0 (ImplVtable, 1);
	vtable->arg = arg;
	vtable->sample = sample;
	vtable->update = update;
	return vtable;
}

MonoPerfCounters *mono_perfcounters = NULL;
static MonoSharedArea *shared_area = NULL;

typedef struct {
	void *sarea;
	int refcount;
} ExternalSArea;

/* maps a pid to a ExternalSArea pointer */
static GHashTable *pid_to_shared_area = NULL;

static MonoSharedArea *
load_sarea_for_pid (int pid)
{
	ExternalSArea *data;
	MonoSharedArea *area = NULL;

	perfctr_lock ();
	if (pid_to_shared_area == NULL)
		pid_to_shared_area = g_hash_table_new (NULL, NULL);
	data = g_hash_table_lookup (pid_to_shared_area, GINT_TO_POINTER (pid));
	if (!data) {
		area = mono_shared_area_for_pid (GINT_TO_POINTER (pid));
		if (area) {
			data = g_new (ExternalSArea, 1);
			data->sarea = area;
			data->refcount = 1;
			g_hash_table_insert (pid_to_shared_area, GINT_TO_POINTER (pid), data);
		}
	} else {
		area = data->sarea;
		data->refcount ++;
	}
	perfctr_unlock ();
	return area;
}

static void
predef_cleanup (ImplVtable *vtable)
{
	PredefVtable *vt = (PredefVtable*)vtable;
	ExternalSArea *data;
	perfctr_lock ();
	if (!pid_to_shared_area) {
		perfctr_unlock ();
		return;
	}
	data = g_hash_table_lookup (pid_to_shared_area, GINT_TO_POINTER (vt->pid));
	if (data) {
		data->refcount--;
		if (!data->refcount) {
			g_hash_table_remove (pid_to_shared_area, GINT_TO_POINTER (vt->pid));
			mono_shared_area_unload (data->sarea);
			g_free (data);
		}
	}
	perfctr_unlock ();
}

void
mono_perfcounters_init (void)
{
	int d_offset = G_STRUCT_OFFSET (MonoSharedArea, data);
	d_offset += 7;
	d_offset &= ~7;

	InitializeCriticalSection (&perfctr_mutex);

	shared_area = mono_shared_area ();
	shared_area->counters_start = G_STRUCT_OFFSET (MonoSharedArea, counters);
	shared_area->counters_size = sizeof (MonoPerfCounters);
	shared_area->data_start = d_offset;
	shared_area->size = 4096;
	mono_perfcounters = &shared_area->counters;
}

static int
perfctr_type_compress (int type)
{
	int i;
	for (i = 0; i < G_N_ELEMENTS (simple_type_to_type); ++i) {
		if (simple_type_to_type [i] == type)
			return i;
	}
	/* NumberOfItems32 */
	return 2;
}

static unsigned char*
shared_data_find_room (int size)
{
	unsigned char *p = (unsigned char *)shared_area + shared_area->data_start;
	unsigned char *end = (unsigned char *)shared_area + shared_area->size;

	size += 3;
	size &= ~3;
	while (p < end) {
		unsigned short *next;
		if (*p == FTYPE_END) {
			if (size < (end - p))
				return p;
			return NULL;
		}
		if (p + 4 > end)
			return NULL;
		next = (unsigned short*)(p + 2);
		if (*p == FTYPE_DELETED) {
			/* we reuse only if it's the same size */
			if (*next == size) {
				return p;
			}
		}
		p += *next;
	}
	return NULL;
}

typedef gboolean (*SharedFunc) (SharedHeader *header, void *data);

static void
foreach_shared_item (SharedFunc func, void *data)
{
	unsigned char *p = (unsigned char *)shared_area + shared_area->data_start;
	unsigned char *end = (unsigned char *)shared_area + shared_area->size;

	while (p < end) {
		unsigned short *next;
		if (p + 4 > end)
			return;
		next = (unsigned short*)(p + 2);
		if (!func ((SharedHeader*)p, data))
			return;
		if (*p == FTYPE_END)
			return;
		p += *next;
	}
}

static int
mono_string_compare_ascii (MonoString *str, const char *ascii_str)
{
	/* FIXME: make this case insensitive */
	guint16 *strc = mono_string_chars (str);
	while (*strc == *ascii_str++) {
		if (*strc == 0)
			return 0;
		strc++;
	}
	return *strc - *(const unsigned char *)(ascii_str - 1);
}

typedef struct {
	MonoString *name;
	SharedCategory *cat;
} CatSearch;

static gboolean
category_search (SharedHeader *header, void *data)
{
	CatSearch *search = data;
	if (header->ftype == FTYPE_CATEGORY) {
		SharedCategory *cat = (SharedCategory*)header;
		if (mono_string_compare_ascii (search->name, cat->name) == 0) {
			search->cat = cat;
			return FALSE;
		}
	}
	return TRUE;
}

static SharedCategory*
find_custom_category (MonoString *name)
{
	CatSearch search;
	search.name = name;
	search.cat = NULL;
	foreach_shared_item (category_search, &search);
	return search.cat;
}

static gboolean
category_collect (SharedHeader *header, void *data)
{
	GSList **list = data;
	if (header->ftype == FTYPE_CATEGORY) {
		*list = g_slist_prepend (*list, header);
	}
	return TRUE;
}

static GSList*
get_custom_categories (void) {
	GSList *list = NULL;
	foreach_shared_item (category_collect, &list);
	return list;
}

static char*
custom_category_counters (SharedCategory* cat)
{
	char *p = cat->name + strlen (cat->name) + 1;
	p += strlen (p) + 1; /* skip category help */
	return p;
}

static SharedCounter*
find_custom_counter (SharedCategory* cat, MonoString *name)
{
	int i;
	char *p = custom_category_counters (cat);
	for (i = 0; i < cat->num_counters; ++i) {
		SharedCounter *counter = (SharedCounter*)p;
		if (mono_string_compare_ascii (name, counter->name) == 0)
			return counter;
		p += 1 + strlen (p + 1) + 1; /* skip counter type and name */
		p += strlen (p) + 1; /* skip counter help */
	}
	return NULL;
}

static char*
custom_category_help (SharedCategory* cat)
{
	return cat->name + strlen (cat->name) + 1;
}

static const CounterDesc*
get_counter_in_category (const CategoryDesc *desc, MonoString *counter)
{
	const CounterDesc *cdesc = &predef_counters [desc->first_counter];
	const CounterDesc *end = &predef_counters [desc [1].first_counter];
	for (; cdesc < end; ++cdesc) {
		if (mono_string_compare_ascii (counter, cdesc->name) == 0)
			return cdesc;
	}
	return NULL;
}

/* fill the info in sample (except the raw value) */
static void
fill_sample (MonoCounterSample *sample)
{
	sample->timeStamp = mono_100ns_ticks ();
	sample->timeStamp100nSec = sample->timeStamp;
	sample->counterTimeStamp = sample->timeStamp;
	sample->counterFrequency = 10000000;
	sample->systemFrequency = 10000000;
	// the real basevalue needs to be get from a different counter...
	sample->baseValue = 0;
}

static int
id_from_string (MonoString *instance)
{
	int id = -1;
	if (mono_string_length (instance)) {
		char *id_str = mono_string_to_utf8 (instance);
		char *end;
		id = strtol (id_str, &end, 0);
		g_free (id_str);
		if (end == id_str)
			return -1;
	}
	return id;
}

static void
get_cpu_times (int cpu_id, gint64 *user, gint64 *systemt, gint64 *irq, gint64 *sirq, gint64 *idle)
{
	char buf [256];
	char *s;
	int hz = 100 * 2; // 2 numprocs
	long long unsigned int user_ticks, nice_ticks, system_ticks, idle_ticks, iowait_ticks, irq_ticks, sirq_ticks;
	FILE *f = fopen ("/proc/stat", "r");
	if (!f)
		return;
	while ((s = fgets (buf, sizeof (buf), f))) {
		char *data = NULL;
		if (cpu_id < 0 && strncmp (s, "cpu", 3) == 0 && g_ascii_isspace (s [3])) {
			data = s + 4;
		} else if (cpu_id >= 0 && strncmp (s, "cpu", 3) == 0 && strtol (s + 3, &data, 10) == cpu_id) {
			if (data == s + 3)
				continue;
			data++;
		} else {
			continue;
		}
		sscanf (data, "%Lu %Lu %Lu %Lu %Lu %Lu %Lu", &user_ticks, &nice_ticks, &system_ticks, &idle_ticks, &iowait_ticks, &irq_ticks, &sirq_ticks);
	}
	fclose (f);

	if (user)
		*user = (user_ticks + nice_ticks) * 10000000 / hz;
	if (systemt)
		*systemt = (system_ticks) * 10000000 / hz;
	if (irq)
		*irq = (irq_ticks) * 10000000 / hz;
	if (sirq)
		*sirq = (sirq_ticks) * 10000000 / hz;
	if (idle)
		*idle = (idle_ticks) * 10000000 / hz;
}

static MonoBoolean
get_cpu_counter (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample *sample)
{
	gint64 value = 0;
	int id = GPOINTER_TO_INT (vtable->arg);
	int pid = id >> 5;
	id &= 0x1f;
	if (!only_value) {
		fill_sample (sample);
		sample->baseValue = 1;
	}
	sample->counterType = predef_counters [predef_categories [CATEGORY_PROC].first_counter + id].type;
	switch (id) {
	case COUNTER_CPU_USER_TIME:
		get_cpu_times (pid, &value, NULL, NULL, NULL, NULL);
		sample->rawValue = value;
		return TRUE;
	case COUNTER_CPU_PRIV_TIME:
		get_cpu_times (pid, NULL, &value, NULL, NULL, NULL);
		sample->rawValue = value;
		return TRUE;
	case COUNTER_CPU_INTR_TIME:
		get_cpu_times (pid, NULL, NULL, &value, NULL, NULL);
		sample->rawValue = value;
		return TRUE;
	case COUNTER_CPU_DCP_TIME:
		get_cpu_times (pid, NULL, NULL, NULL, &value, NULL);
		sample->rawValue = value;
		return TRUE;
	case COUNTER_CPU_PROC_TIME:
		get_cpu_times (pid, NULL, NULL, NULL, NULL, &value);
		sample->rawValue = value;
		return TRUE;
	}
	return FALSE;
}

static void*
cpu_get_impl (MonoString* counter, MonoString* instance, int *type, MonoBoolean *custom)
{
	int id = id_from_string (instance) << 5;
	const CounterDesc *cdesc;
	*custom = FALSE;
	/* increase the shift above and the mask also in the implementation functions */
	//g_assert (32 > desc [1].first_counter - desc->first_counter);
	if ((cdesc = get_counter_in_category (&predef_categories [CATEGORY_CPU], counter))) {
		*type = cdesc->type;
		return create_vtable (GINT_TO_POINTER (id | cdesc->id), get_cpu_counter, NULL);
	}
	return NULL;
}

/*
 * /proc/pid/stat format:
 * pid (cmdname) S 
 * 	[0] ppid pgid sid tty_nr tty_pgrp flags min_flt cmin_flt maj_flt cmaj_flt
 * 	[10] utime stime cutime cstime prio nice threads start_time vsize rss
 * 	[20] rsslim start_code end_code start_stack esp eip pending blocked sigign sigcatch
 * 	[30] wchan 0 0 exit_signal cpu rt_prio policy
 */

static gint64
get_process_time (int pid, int pos, int sum)
{
	char buf [512];
	char *s, *end;
	FILE *f;
	int len, i;
	gint64 value;

	g_snprintf (buf, sizeof (buf), "/proc/%d/stat", pid);
	f = fopen (buf, "r");
	if (!f)
		return 0;
	len = fread (buf, 1, sizeof (buf), f);
	fclose (f);
	if (len <= 0)
		return 0;
	s = strchr (buf, ')');
	if (!s)
		return 0;
	s++;
	while (g_ascii_isspace (*s)) s++;
	if (!*s)
		return 0;
	/* skip the status char */
	while (*s && !g_ascii_isspace (*s)) s++;
	if (!*s)
		return 0;
	for (i = 0; i < pos; ++i) {
		while (g_ascii_isspace (*s)) s++;
		if (!*s)
			return 0;
		while (*s && !g_ascii_isspace (*s)) s++;
		if (!*s)
			return 0;
	}
	/* we are finally at the needed item */
	value = strtoul (s, &end, 0);
	/* add also the following value */
	if (sum) {
		while (g_ascii_isspace (*s)) s++;
		if (!*s)
			return 0;
		value += strtoul (s, &end, 0);
	}
	return value;
}

static gint64
get_pid_stat_item (int pid, const char *item)
{
	char buf [256];
	char *s;
	FILE *f;
	int len = strlen (item);

	g_snprintf (buf, sizeof (buf), "/proc/%d/status", pid);
	f = fopen (buf, "r");
	if (!f)
		return 0;
	while ((s = fgets (buf, sizeof (buf), f))) {
		if (*item != *buf)
			continue;
		if (strncmp (buf, item, len))
			continue;
		if (buf [len] != ':')
			continue;
		fclose (f);
		return atoi (buf + len + 1);
	}
	fclose (f);
	return 0;
}

static MonoBoolean
get_process_counter (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample *sample)
{
	int id = GPOINTER_TO_INT (vtable->arg);
	int pid = id >> 5;
	if (pid < 0)
		return FALSE;
	id &= 0x1f;
	if (!only_value) {
		fill_sample (sample);
		sample->baseValue = 1;
	}
	sample->counterType = predef_counters [predef_categories [CATEGORY_PROC].first_counter + id].type;
	switch (id) {
	case COUNTER_PROC_USER_TIME:
		sample->rawValue = get_process_time (pid, 12, FALSE);
		return TRUE;
	case COUNTER_PROC_PRIV_TIME:
		sample->rawValue = get_process_time (pid, 13, FALSE);
		return TRUE;
	case COUNTER_PROC_PROC_TIME:
		sample->rawValue = get_process_time (pid, 12, TRUE);
		return TRUE;
	case COUNTER_PROC_THREADS:
		sample->rawValue = get_pid_stat_item (pid, "Threads");
		return TRUE;
	case COUNTER_PROC_VBYTES:
		sample->rawValue = get_pid_stat_item (pid, "VmSize") * 1024;
		return TRUE;
	case COUNTER_PROC_WSET:
		sample->rawValue = get_pid_stat_item (pid, "VmRSS") * 1024;
		return TRUE;
	case COUNTER_PROC_PBYTES:
		sample->rawValue = get_pid_stat_item (pid, "VmData") * 1024;
		return TRUE;
	}
	return FALSE;
}

static void*
process_get_impl (MonoString* counter, MonoString* instance, int *type, MonoBoolean *custom)
{
	int id = id_from_string (instance) << 5;
	const CounterDesc *cdesc;
	*custom = FALSE;
	/* increase the shift above and the mask also in the implementation functions */
	//g_assert (32 > desc [1].first_counter - desc->first_counter);
	if ((cdesc = get_counter_in_category (&predef_categories [CATEGORY_PROC], counter))) {
		*type = cdesc->type;
		return create_vtable (GINT_TO_POINTER (id | cdesc->id), get_process_counter, NULL);
	}
	return NULL;
}

static MonoBoolean
mono_mem_counter (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample *sample)
{
	int id = GPOINTER_TO_INT (vtable->arg);
	if (!only_value) {
		fill_sample (sample);
		sample->baseValue = 1;
	}
	sample->counterType = predef_counters [predef_categories [CATEGORY_MONO_MEM].first_counter + id].type;
	switch (id) {
	case COUNTER_MEM_NUM_OBJECTS:
		sample->rawValue = mono_stats.new_object_count;
		return TRUE;
	}
	return FALSE;
}

static void*
mono_mem_get_impl (MonoString* counter, MonoString* instance, int *type, MonoBoolean *custom)
{
	const CounterDesc *cdesc;
	*custom = FALSE;
	if ((cdesc = get_counter_in_category (&predef_categories [CATEGORY_MONO_MEM], counter))) {
		*type = cdesc->type;
		return create_vtable (GINT_TO_POINTER (cdesc->id), mono_mem_counter, NULL);
	}
	return NULL;
}

static MonoBoolean
predef_readonly_counter (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample *sample)
{
	PredefVtable *vt = (PredefVtable *)vtable;
	int cat_id = GPOINTER_TO_INT (vtable->arg);
	int id = cat_id >> 16;
	cat_id &= 0xffff;
	if (!only_value) {
		fill_sample (sample);
		sample->baseValue = 1;
	}
	sample->counterType = predef_counters [predef_categories [cat_id].first_counter + id].type;
	switch (cat_id) {
	case CATEGORY_EXC:
		switch (id) {
		case COUNTER_EXC_THROWN:
			sample->rawValue = vt->counters->exceptions_thrown;
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static ImplVtable*
predef_vtable (void *arg, MonoString *instance)
{
	MonoSharedArea *area;
	PredefVtable *vtable;
	char *pids = mono_string_to_utf8 (instance);
	int pid;

	pid = atoi (pids);
	g_free (pids);
	area = load_sarea_for_pid (pid);
	if (!area)
		return NULL;

	vtable = g_new (PredefVtable, 1);
	vtable->vtable.arg = arg;
	vtable->vtable.sample = predef_readonly_counter;
	vtable->vtable.cleanup = predef_cleanup;
	vtable->counters = &area->counters;
	vtable->pid = pid;

	return vtable;
}

/* consider storing the pointer directly in vtable->arg, so the runtime overhead is lower:
 * this needs some way to set sample->counterType as well, though.
 */
static MonoBoolean
predef_writable_counter (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample *sample)
{
	int cat_id = GPOINTER_TO_INT (vtable->arg);
	int id = cat_id >> 16;
	cat_id &= 0xffff;
	if (!only_value) {
		fill_sample (sample);
		sample->baseValue = 1;
	}
	sample->counterType = predef_counters [predef_categories [cat_id].first_counter + id].type;
	switch (cat_id) {
	case CATEGORY_ASPNET:
		switch (id) {
		case COUNTER_ASPNET_REQ_Q:
			sample->rawValue = mono_perfcounters->aspnet_requests_queued;
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static gint64
predef_writable_update (ImplVtable *vtable, MonoBoolean do_incr, gint64 value)
{
	guint32 *ptr = NULL;
	int cat_id = GPOINTER_TO_INT (vtable->arg);
	int id = cat_id >> 16;
	cat_id &= 0xffff;
	switch (cat_id) {
	case CATEGORY_ASPNET:
		switch (id) {
		case COUNTER_ASPNET_REQ_Q: ptr = &mono_perfcounters->aspnet_requests_queued; break;
		}
		break;
	}
	if (ptr) {
		if (do_incr) {
			/* FIXME: we need to do this atomically */
			*ptr += value;
			return *ptr;
		}
		/* this can be non-atomic */
		*ptr = value;
		return value;
	}
	return 0;
}

static void*
predef_writable_get_impl (int cat, MonoString* counter, MonoString* instance, int *type, MonoBoolean *custom)
{
	const CounterDesc *cdesc;
	*custom = TRUE;
	if ((cdesc = get_counter_in_category (&predef_categories [cat], counter))) {
		*type = cdesc->type;
		if (instance == NULL || mono_string_compare_ascii (instance, "") == 0)
			return create_vtable (GINT_TO_POINTER ((cdesc->id << 16) | cat), predef_writable_counter, predef_writable_update);
		else
			return predef_vtable (GINT_TO_POINTER ((cdesc->id << 16) | cat), instance);
	}
	return NULL;
}

static MonoBoolean
custom_writable_counter (ImplVtable *vtable, MonoBoolean only_value, MonoCounterSample *sample)
{
	SharedCounter *scounter = vtable->arg;
	if (!only_value) {
		fill_sample (sample);
		sample->baseValue = 1;
	}
	sample->counterType = simple_type_to_type [scounter->type];
	/* FIXME */
	sample->rawValue = 0;
	return TRUE;
}

static gint64
custom_writable_update (ImplVtable *vtable, MonoBoolean do_incr, gint64 value)
{
	SharedCounter *scounter = vtable->arg;
	/* FIXME */
	guint32 *ptr = NULL;
	if (ptr) {
		if (do_incr) {
			/* FIXME: we need to do this atomically */
			*ptr += value;
			return *ptr;
		}
		/* this can be non-atomic */
		*ptr = value;
		return value;
	}
	return 0;
}

static void*
custom_get_impl (SharedCategory *cat, MonoString* counter, MonoString* instance, int *type)
{
	SharedCounter *scounter;

	scounter = find_custom_counter (cat, counter);
	if (!scounter)
		return NULL;
	*type = simple_type_to_type [scounter->type];
	/* FIXME: use instance */
	return create_vtable (scounter, custom_writable_counter, custom_writable_update);
}

static const CategoryDesc*
find_category (MonoString *category)
{
	int i;
	for (i = 0; i < NUM_CATEGORIES; ++i) {
		if (mono_string_compare_ascii (category, predef_categories [i].name) == 0)
			return &predef_categories [i];
	}
	return NULL;
}

void*
mono_perfcounter_get_impl (MonoString* category, MonoString* counter, MonoString* instance,
		MonoString* machine, int *type, MonoBoolean *custom)
{
	const CategoryDesc *cdesc;
	/* no support for counters on other machines */
	if (mono_string_compare_ascii (machine, "."))
		return NULL;
	cdesc = find_category (category);
	if (!cdesc) {
		SharedCategory *scat = find_custom_category (category);
		if (!scat)
			return NULL;
		*custom = TRUE;
		return custom_get_impl (scat, counter, instance, type);
	}
	switch (cdesc->id) {
	case CATEGORY_CPU:
		return cpu_get_impl (counter, instance, type, custom);
	case CATEGORY_PROC:
		return process_get_impl (counter, instance, type, custom);
	case CATEGORY_MONO_MEM:
		return mono_mem_get_impl (counter, instance, type, custom);
	case CATEGORY_JIT:
	case CATEGORY_EXC:
	case CATEGORY_ASPNET:
		return predef_writable_get_impl (cdesc->id, counter, instance, type, custom);
	}
	return NULL;
}

MonoBoolean
mono_perfcounter_get_sample (void *impl, MonoBoolean only_value, MonoCounterSample *sample)
{
	ImplVtable *vtable = impl;
	if (vtable && vtable->sample)
		return vtable->sample (vtable, only_value, sample);
	return FALSE;
}

gint64
mono_perfcounter_update_value (void *impl, MonoBoolean do_incr, gint64 value)
{
	ImplVtable *vtable = impl;
	if (vtable && vtable->update)
		return vtable->update (vtable, do_incr, value);
	return 0;
}

void
mono_perfcounter_free_data (void *impl)
{
	ImplVtable *vtable = impl;
	if (vtable && vtable->cleanup)
		vtable->cleanup (vtable);
	g_free (impl);
}

/* Category icalls */
MonoBoolean
mono_perfcounter_category_del (MonoString *name)
{
	const CategoryDesc *cdesc;
	SharedCategory *cat;
	cdesc = find_category (name);
	/* can't delete a predefined category */
	if (cdesc)
		return FALSE;
	perfctr_lock ();
	cat = find_custom_category (name);
	/* FIXME: check the semantics, if deleting a category means also deleting the instances */
	if (!cat || cat->num_instances) {
		perfctr_unlock ();
		return FALSE;
	}
	cat->header.ftype = FTYPE_DELETED;
	perfctr_unlock ();
	return TRUE;
}

MonoString*
mono_perfcounter_category_help (MonoString *category, MonoString *machine)
{
	const CategoryDesc *cdesc;
	/* no support for counters on other machines */
	if (mono_string_compare_ascii (machine, "."))
		return NULL;
	cdesc = find_category (category);
	if (!cdesc) {
		SharedCategory *scat = find_custom_category (category);
		if (!scat)
			return NULL;
		return mono_string_new (mono_domain_get (), custom_category_help (scat));
	}
	return mono_string_new (mono_domain_get (), cdesc->help);
}

/*
 * Check if the category named @category exists on @machine. If @counter is not NULL, return
 * TRUE only if a counter with that name exists in the category.
 */
MonoBoolean
mono_perfcounter_category_exists (MonoString *counter, MonoString *category, MonoString *machine)
{
	const CategoryDesc *cdesc;
	/* no support for counters on other machines */
	if (mono_string_compare_ascii (machine, "."))
		return FALSE;
	cdesc = find_category (category);
	if (!cdesc) {
		SharedCategory *scat = find_custom_category (category);
		if (!scat)
			return FALSE;
		/* counter is allowed to be null */
		if (!counter)
			return TRUE;
		/* search through the custom category */
		return find_custom_counter (scat, counter) != NULL;
	}
	/* counter is allowed to be null */
	if (!counter)
		return TRUE;
	if (get_counter_in_category (cdesc, counter))
		return TRUE;
	return FALSE;
}

/* C map of the type with the same name */
typedef struct {
	MonoObject object;
	MonoString *help;
	MonoString *name;
	int type;
} CounterCreationData;

/*
 * Since we'll keep a copy of the category per-process, we should also make sure
 * categories with the same name are compatible.
 */
MonoBoolean
mono_perfcounter_create (MonoString *category, MonoString *help, int type, MonoArray *items)
{
	int result = FALSE;
	int i, size;
	int num_counters = mono_array_length (items);
	int counters_data_size;
	char *name = mono_string_to_utf8 (category);
	char *chelp = mono_string_to_utf8 (help);
	char **counter_info;
	unsigned char *ptr;
	char *p;
	SharedCategory *cat;

	counter_info = g_new0 (char*, num_counters * 2);
	/* calculate the size we need structure size + name/help + 2 0 string terminators */
	size = G_STRUCT_OFFSET (SharedCategory, name) + strlen (name) + strlen (chelp) + 2;
	for (i = 0; i < num_counters; ++i) {
		CounterCreationData *data = mono_array_get (items, CounterCreationData*, i);
		counter_info [i * 2] = mono_string_to_utf8 (data->name);
		counter_info [i * 2 + 1] = mono_string_to_utf8 (data->help);
		size += 3; /* type and two 0 string terminators */
	}
	for (i = 0; i < num_counters * 2; ++i) {
		if (!counter_info [i])
			goto failure;
		size += strlen (counter_info [i]);
	}
	counters_data_size = num_counters * 8; /* optimize for size later */
	if (size > 65535)
		goto failure;
	perfctr_lock ();
	ptr = shared_data_find_room (size);
	if (!ptr) {
		perfctr_unlock ();
		goto failure;
	}
	cat = (SharedCategory*)ptr;
	cat->header.extra = type;
	cat->header.size = size;
	cat->num_counters = num_counters;
	cat->counters_data_size = counters_data_size;
	/* now copy the vaiable data */
	p = cat->name;
	strcpy (p, name);
	p += strlen (name) + 1;
	strcpy (p, chelp);
	p += strlen (chelp) + 1;
	for (i = 0; i < num_counters; ++i) {
		CounterCreationData *data = mono_array_get (items, CounterCreationData*, i);
		*p++ = perfctr_type_compress (data->type);
		strcpy (p, counter_info [i * 2]);
		p += strlen (counter_info [i * 2]) + 1;
		strcpy (p, counter_info [i * 2 + 1]);
		p += strlen (counter_info [i * 2 + 1]) + 1;
	}
	cat->header.ftype = FTYPE_CATEGORY;

	perfctr_unlock ();
	result = TRUE;
failure:
	for (i = 0; i < num_counters * 2; ++i) {
		g_free (counter_info [i]);
	}
	g_free (counter_info);
	g_free (name);
	g_free (chelp);
	return result;
}

int
mono_perfcounter_instance_exists (MonoString *instance, MonoString *category, MonoString *machine)
{
	const CategoryDesc *cdesc;
	/* no support for counters on other machines */
	if (mono_string_compare_ascii (machine, "."))
		return FALSE;
	cdesc = find_category (category);
	if (!cdesc)
		return FALSE;
	return FALSE;
}

MonoArray*
mono_perfcounter_category_names (MonoString *machine)
{
	int i;
	MonoArray *res;
	MonoDomain *domain = mono_domain_get ();
	GSList *custom_categories, *tmp;
	/* no support for counters on other machines */
	if (mono_string_compare_ascii (machine, "."))
		return mono_array_new (domain, mono_get_string_class (), 0);
	perfctr_lock ();
	custom_categories = get_custom_categories ();
	res = mono_array_new (domain, mono_get_string_class (), NUM_CATEGORIES + g_slist_length (custom_categories));
	for (i = 0; i < NUM_CATEGORIES; ++i) {
		const CategoryDesc *cdesc = &predef_categories [i];
		mono_array_setref (res, i, mono_string_new (domain, cdesc->name));
	}
	for (tmp = custom_categories; tmp; tmp = tmp->next) {
		SharedCategory *scat = tmp->data;
		mono_array_setref (res, i, mono_string_new (domain, scat->name));
		i++;
	}
	perfctr_unlock ();
	g_slist_free (custom_categories);
	return res;
}

MonoArray*
mono_perfcounter_counter_names (MonoString *category, MonoString *machine)
{
	int i;
	SharedCategory *scat;
	const CategoryDesc *cdesc;
	MonoArray *res;
	MonoDomain *domain = mono_domain_get ();
	/* no support for counters on other machines */
	if (mono_string_compare_ascii (machine, "."))
		return mono_array_new (domain, mono_get_string_class (), 0);
	cdesc = find_category (category);
	if (cdesc) {
		res = mono_array_new (domain, mono_get_string_class (), cdesc [1].first_counter - cdesc->first_counter);
		for (i = cdesc->first_counter; i < cdesc [1].first_counter; ++i) {
			const CounterDesc *desc = &predef_counters [i];
			mono_array_setref (res, i - cdesc->first_counter, mono_string_new (domain, desc->name));
		}
		return res;
	}
	perfctr_lock ();
	scat = find_custom_category (category);
	if (scat) {
		char *p = custom_category_counters (scat);
		int i;
		res = mono_array_new (domain, mono_get_string_class (), scat->num_counters);
		for (i = 0; i < scat->num_counters; ++i) {
			mono_array_setref (res, i, mono_string_new (domain, p + 1));
			p += 1 + strlen (p + 1) + 1; /* skip counter type and name */
			p += strlen (p) + 1; /* skip counter help */
		}
		perfctr_unlock ();
		return res;
	}
	perfctr_unlock ();
	return mono_array_new (domain, mono_get_string_class (), 0);
}

MonoArray*
mono_perfcounter_instance_names (MonoString *category, MonoString *machine)
{
	if (mono_string_compare_ascii (machine, "."))
		return mono_array_new (mono_domain_get (), mono_get_string_class (), 0);
	return mono_array_new (mono_domain_get (), mono_get_string_class (), 0);
}

