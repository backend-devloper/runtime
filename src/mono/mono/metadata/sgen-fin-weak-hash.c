/*
 * The finalizable hash has the object as the key, the 
 * disappearing_link hash, has the link address as key.
 *
 * Copyright 2011 Xamarin Inc.
 */
static SgenHashTable minor_finalizable_hash = SGEN_HASH_TABLE_INIT (INTERNAL_MEM_FIN_TABLE, INTERNAL_MEM_FINALIZE_ENTRY, 0, (SgenHashFunc)mono_object_hash);
static SgenHashTable major_finalizable_hash = SGEN_HASH_TABLE_INIT (INTERNAL_MEM_FIN_TABLE, INTERNAL_MEM_FINALIZE_ENTRY, 0, (SgenHashFunc)mono_object_hash);

static SgenHashTable*
get_finalize_entry_hash_table (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return &minor_finalizable_hash;
	case GENERATION_OLD: return &major_finalizable_hash;
	default: g_assert_not_reached ();
	}
}

/* LOCKING: requires that the GC lock is held */
static void
finalize_in_range (CopyOrMarkObjectFunc copy_func, char *start, char *end, int generation, GrayQueue *queue)
{
	SgenHashTable *hash_table = get_finalize_entry_hash_table (generation);
	MonoObject *object;
	gpointer dummy;

	if (no_finalize)
		return;
	SGEN_HASH_TABLE_FOREACH (hash_table, object, dummy) {
		if ((char*)object >= start && (char*)object < end && !major_collector.is_object_live ((char*)object)) {
			gboolean is_fin_ready = object_is_fin_ready (object);
			MonoObject *copy = object;
			copy_func ((void**)&copy, queue);
			if (is_fin_ready) {
				/* remove and put in fin_ready_list */
				SGEN_HASH_TABLE_FOREACH_REMOVE;
				num_ready_finalizers++;
				queue_finalization_entry (copy);
				bridge_register_finalized_object (copy);
				/* Make it survive */
				DEBUG (5, fprintf (gc_debug_file, "Queueing object for finalization: %p (%s) (was at %p) (%d/%d)\n", copy, safe_name (copy), object, num_ready_finalizers, mono_sgen_hash_table_num_entries (hash_table)));
				continue;
			} else {
				if (hash_table == &minor_finalizable_hash && !ptr_in_nursery (copy)) {
					/* remove from the list */
					SGEN_HASH_TABLE_FOREACH_REMOVE;

					/* insert it into the major hash */
					mono_sgen_hash_table_replace (&major_finalizable_hash, copy, NULL);

					DEBUG (5, fprintf (gc_debug_file, "Promoting finalization of object %p (%s) (was at %p) to major table\n", copy, safe_name (copy), object));

					continue;
				} else {
					/* update pointer */
					DEBUG (5, fprintf (gc_debug_file, "Updating object for finalization: %p (%s) (was at %p)\n", copy, safe_name (copy), object));
					SGEN_HASH_TABLE_FOREACH_SET_KEY (copy);
				}
			}
		}
	} SGEN_HASH_TABLE_FOREACH_END;
}

/* LOCKING: requires that the GC lock is held */
static void
register_for_finalization (MonoObject *obj, void *user_data, int generation)
{
	SgenHashTable *hash_table = get_finalize_entry_hash_table (generation);

	if (no_finalize)
		return;

	g_assert (user_data == NULL || user_data == mono_gc_run_finalize);

	if (user_data) {
		if (mono_sgen_hash_table_replace (hash_table, obj, NULL))
			DEBUG (5, fprintf (gc_debug_file, "Added finalizer for object: %p (%s) (%d) to %s table\n", obj, obj->vtable->klass->name, hash_table->num_entries, generation_name (generation)));
	} else {
		if (mono_sgen_hash_table_remove (hash_table, obj))
			DEBUG (5, fprintf (gc_debug_file, "Removed finalizer for object: %p (%s) (%d)\n", obj, obj->vtable->klass->name, hash_table->num_entries));
	}
}

#define STAGE_ENTRY_FREE	0
#define STAGE_ENTRY_BUSY	1
#define STAGE_ENTRY_USED	2

typedef struct {
	gint32 state;
	MonoObject *obj;
	void *user_data;
} StageEntry;

#define NUM_FIN_STAGE_ENTRIES	1024

static volatile gint32 next_fin_stage_entry = 0;
static StageEntry fin_stage_entries [NUM_FIN_STAGE_ENTRIES];

/* LOCKING: requires that the GC lock is held */
static void
process_stage_entries (int num_entries, volatile gint32 *next_entry, StageEntry *entries, void (*process_func) (MonoObject*, void*))
{
	int i;
	int num_registered = 0;
	int num_busy = 0;

	for (i = 0; i < num_entries; ++i) {
		gint32 state = entries [i].state;

		if (state == STAGE_ENTRY_BUSY)
			++num_busy;

		if (state != STAGE_ENTRY_USED ||
				InterlockedCompareExchange (&entries [i].state, STAGE_ENTRY_BUSY, STAGE_ENTRY_USED) != STAGE_ENTRY_USED) {
			continue;
		}

		process_func (entries [i].obj, entries [i].user_data);

		entries [i].obj = NULL;
		entries [i].user_data = NULL;

		mono_memory_write_barrier ();

		entries [i].state = STAGE_ENTRY_FREE;

		++num_registered;
	}

	*next_entry = 0;

	/* g_print ("stage busy %d reg %d\n", num_busy, num_registered); */
}

static gboolean
add_stage_entry (int num_entries, volatile gint32 *next_entry, StageEntry *entries, MonoObject *obj, void *user_data)
{
	gint32 index;

	do {
		do {
			index = *next_entry;
			if (index >= num_entries)
				return FALSE;
		} while (InterlockedCompareExchange (next_entry, index + 1, index) != index);

		/*
		 * We don't need a write barrier here.  *next_entry is just a
		 * help for finding an index, its value is irrelevant for
		 * correctness.
		 */
	} while (entries [index].state != STAGE_ENTRY_FREE ||
			InterlockedCompareExchange (&entries [index].state, STAGE_ENTRY_BUSY, STAGE_ENTRY_FREE) != STAGE_ENTRY_FREE);

	entries [index].obj = obj;
	entries [index].user_data = user_data;

	mono_memory_write_barrier ();

	entries [index].state = STAGE_ENTRY_USED;

	return TRUE;
}

/* LOCKING: requires that the GC lock is held */
static void
process_fin_stage_entry (MonoObject *obj, void *user_data)
{
	if (ptr_in_nursery (obj))
		register_for_finalization (obj, user_data, GENERATION_NURSERY);
	else
		register_for_finalization (obj, user_data, GENERATION_OLD);
}

/* LOCKING: requires that the GC lock is held */
static void
process_fin_stage_entries (void)
{
	process_stage_entries (NUM_FIN_STAGE_ENTRIES, &next_fin_stage_entry, fin_stage_entries, process_fin_stage_entry);
}

void
mono_gc_register_for_finalization (MonoObject *obj, void *user_data)
{
	while (!add_stage_entry (NUM_FIN_STAGE_ENTRIES, &next_fin_stage_entry, fin_stage_entries, obj, user_data)) {
		LOCK_GC;
		process_fin_stage_entries ();
		UNLOCK_GC;
	}
}

/* LOCKING: requires that the GC lock is held */
static int
finalizers_for_domain (MonoDomain *domain, MonoObject **out_array, int out_size,
	SgenHashTable *hash_table)
{
	MonoObject *object;
	gpointer dummy;
	int count;

	if (no_finalize || !out_size || !out_array)
		return 0;
	count = 0;
	SGEN_HASH_TABLE_FOREACH (hash_table, object, dummy) {
		if (mono_object_domain (object) == domain) {
			/* remove and put in out_array */
			SGEN_HASH_TABLE_FOREACH_REMOVE;
			out_array [count ++] = object;
			DEBUG (5, fprintf (gc_debug_file, "Collecting object for finalization: %p (%s) (%d/%d)\n", object, safe_name (object), num_ready_finalizers, mono_sgen_hash_table_num_entries (hash_table)));
			if (count == out_size)
				return count;
			continue;
		}
	} SGEN_HASH_TABLE_FOREACH_END;
	return count;
}

/**
 * mono_gc_finalizers_for_domain:
 * @domain: the unloading appdomain
 * @out_array: output array
 * @out_size: size of output array
 *
 * Store inside @out_array up to @out_size objects that belong to the unloading
 * appdomain @domain. Returns the number of stored items. Can be called repeteadly
 * until it returns 0.
 * The items are removed from the finalizer data structure, so the caller is supposed
 * to finalize them.
 * @out_array should be on the stack to allow the GC to know the objects are still alive.
 */
int
mono_gc_finalizers_for_domain (MonoDomain *domain, MonoObject **out_array, int out_size)
{
	int result;

	LOCK_GC;
	process_fin_stage_entries ();
	result = finalizers_for_domain (domain, out_array, out_size, &minor_finalizable_hash);
	if (result < out_size) {
		result += finalizers_for_domain (domain, out_array + result, out_size - result,
			&major_finalizable_hash);
	}
	UNLOCK_GC;

	return result;
}

static DisappearingLinkHashTable minor_disappearing_link_hash;
static DisappearingLinkHashTable major_disappearing_link_hash;

static DisappearingLinkHashTable*
get_dislink_hash_table (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return &minor_disappearing_link_hash;
	case GENERATION_OLD: return &major_disappearing_link_hash;
	default: g_assert_not_reached ();
	}
}

/* LOCKING: assumes the GC lock is held */
static void
rehash_dislink (DisappearingLinkHashTable *hash_table)
{
	DisappearingLink **disappearing_link_hash = hash_table->table;
	int disappearing_link_hash_size = hash_table->size;
	int i;
	unsigned int hash;
	DisappearingLink **new_hash;
	DisappearingLink *entry, *next;
	int new_size = g_spaced_primes_closest (hash_table->num_links);

	new_hash = mono_sgen_alloc_internal_dynamic (new_size * sizeof (DisappearingLink*), INTERNAL_MEM_DISLINK_TABLE);
	for (i = 0; i < disappearing_link_hash_size; ++i) {
		for (entry = disappearing_link_hash [i]; entry; entry = next) {
			hash = mono_aligned_addr_hash (entry->link) % new_size;
			next = entry->next;
			entry->next = new_hash [hash];
			new_hash [hash] = entry;
		}
	}
	mono_sgen_free_internal_dynamic (disappearing_link_hash,
			disappearing_link_hash_size * sizeof (DisappearingLink*), INTERNAL_MEM_DISLINK_TABLE);
	hash_table->table = new_hash;
	hash_table->size = new_size;
}

/* LOCKING: assumes the GC lock is held */
static void
add_or_remove_disappearing_link (MonoObject *obj, void **link, int generation)
{
	DisappearingLinkHashTable *hash_table = get_dislink_hash_table (generation);
	DisappearingLink *entry, *prev;
	unsigned int hash;
	DisappearingLink **disappearing_link_hash = hash_table->table;
	int disappearing_link_hash_size = hash_table->size;

	if (hash_table->num_links >= disappearing_link_hash_size * 2) {
		rehash_dislink (hash_table);
		disappearing_link_hash = hash_table->table;
		disappearing_link_hash_size = hash_table->size;
	}
	/* FIXME: add check that link is not in the heap */
	hash = mono_aligned_addr_hash (link) % disappearing_link_hash_size;
	entry = disappearing_link_hash [hash];
	prev = NULL;
	for (; entry; entry = entry->next) {
		/* link already added */
		if (link == entry->link) {
			/* NULL obj means remove */
			if (obj == NULL) {
				if (prev)
					prev->next = entry->next;
				else
					disappearing_link_hash [hash] = entry->next;
				hash_table->num_links--;
				DEBUG (5, fprintf (gc_debug_file, "Removed dislink %p (%d) from %s table\n", entry, hash_table->num_links, generation_name (generation)));
				mono_sgen_free_internal (entry, INTERNAL_MEM_DISLINK);
			}
			return;
		}
		prev = entry;
	}
	if (obj == NULL)
		return;
	entry = mono_sgen_alloc_internal (INTERNAL_MEM_DISLINK);
	entry->link = link;
	entry->next = disappearing_link_hash [hash];
	disappearing_link_hash [hash] = entry;
	hash_table->num_links++;
	DEBUG (5, fprintf (gc_debug_file, "Added dislink %p for object: %p (%s) at %p to %s table\n", entry, obj, obj->vtable->klass->name, link, generation_name (generation)));
}

/* LOCKING: requires that the GC lock is held */
static void
null_link_in_range (CopyOrMarkObjectFunc copy_func, char *start, char *end, int generation, gboolean before_finalization, GrayQueue *queue)
{
	DisappearingLinkHashTable *hash = get_dislink_hash_table (generation);
	DisappearingLink **disappearing_link_hash = hash->table;
	int disappearing_link_hash_size = hash->size;
	DisappearingLink *entry, *prev;
	int i;
	if (!hash->num_links)
		return;
	for (i = 0; i < disappearing_link_hash_size; ++i) {
		prev = NULL;
		for (entry = disappearing_link_hash [i]; entry;) {
			char *object;
			gboolean track = DISLINK_TRACK (entry);

			/*
			 * Tracked references are processed after
			 * finalization handling whereas standard weak
			 * references are processed before.  If an
			 * object is still not marked after finalization
			 * handling it means that it either doesn't have
			 * a finalizer or the finalizer has already run,
			 * so we must null a tracking reference.
			 */
			if (track == before_finalization) {
				prev = entry;
				entry = entry->next;
				continue;
			}

			object = DISLINK_OBJECT (entry);

			if (object >= start && object < end && !major_collector.is_object_live (object)) {
				if (object_is_fin_ready (object)) {
					void **p = entry->link;
					DisappearingLink *old;
					*p = NULL;
					/* remove from list */
					if (prev)
						prev->next = entry->next;
					else
						disappearing_link_hash [i] = entry->next;
					DEBUG (5, fprintf (gc_debug_file, "Dislink nullified at %p to GCed object %p\n", p, object));
					old = entry->next;
					mono_sgen_free_internal (entry, INTERNAL_MEM_DISLINK);
					entry = old;
					hash->num_links--;
					continue;
				} else {
					char *copy = object;
					copy_func ((void**)&copy, queue);

					/* Update pointer if it's moved.  If the object
					 * has been moved out of the nursery, we need to
					 * remove the link from the minor hash table to
					 * the major one.
					 *
					 * FIXME: what if an object is moved earlier?
					 */

					if (hash == &minor_disappearing_link_hash && !ptr_in_nursery (copy)) {
						void **link = entry->link;
						DisappearingLink *old;
						/* remove from list */
						if (prev)
							prev->next = entry->next;
						else
							disappearing_link_hash [i] = entry->next;
						old = entry->next;
						mono_sgen_free_internal (entry, INTERNAL_MEM_DISLINK);
						entry = old;
						hash->num_links--;

						g_assert (copy);
						*link = HIDE_POINTER (copy, track);
						add_or_remove_disappearing_link ((MonoObject*)copy, link, GENERATION_OLD);

						DEBUG (5, fprintf (gc_debug_file, "Upgraded dislink at %p to major because object %p moved to %p\n", link, object, copy));

						continue;
					} else {
						*entry->link = HIDE_POINTER (copy, track);
						DEBUG (5, fprintf (gc_debug_file, "Updated dislink at %p to %p\n", entry->link, DISLINK_OBJECT (entry)));
					}
				}
			}
			prev = entry;
			entry = entry->next;
		}
	}
}

/* LOCKING: requires that the GC lock is held */
static void
null_links_for_domain (MonoDomain *domain, int generation)
{
	DisappearingLinkHashTable *hash = get_dislink_hash_table (generation);
	DisappearingLink **disappearing_link_hash = hash->table;
	int disappearing_link_hash_size = hash->size;
	DisappearingLink *entry, *prev;
	int i;
	for (i = 0; i < disappearing_link_hash_size; ++i) {
		prev = NULL;
		for (entry = disappearing_link_hash [i]; entry; ) {
			char *object = DISLINK_OBJECT (entry);
			if (object && !((MonoObject*)object)->vtable) {
				DisappearingLink *next = entry->next;

				if (prev)
					prev->next = next;
				else
					disappearing_link_hash [i] = next;

				if (*(entry->link)) {
					*(entry->link) = NULL;
					g_warning ("Disappearing link %p not freed", entry->link);
				} else {
					mono_sgen_free_internal (entry, INTERNAL_MEM_DISLINK);
				}

				entry = next;
				continue;
			}
			prev = entry;
			entry = entry->next;
		}
	}
}

/* LOCKING: requires that the GC lock is held */
static void
process_dislink_stage_entry (MonoObject *obj, void *_link)
{
	void **link = _link;

	add_or_remove_disappearing_link (NULL, link, GENERATION_NURSERY);
	add_or_remove_disappearing_link (NULL, link, GENERATION_OLD);
	if (obj) {
		if (ptr_in_nursery (obj))
			add_or_remove_disappearing_link (obj, link, GENERATION_NURSERY);
		else
			add_or_remove_disappearing_link (obj, link, GENERATION_OLD);
	}
}

#define NUM_DISLINK_STAGE_ENTRIES	1024

static volatile gint32 next_dislink_stage_entry = 0;
static StageEntry dislink_stage_entries [NUM_DISLINK_STAGE_ENTRIES];

/* LOCKING: requires that the GC lock is held */
static void
process_dislink_stage_entries (void)
{
	process_stage_entries (NUM_DISLINK_STAGE_ENTRIES, &next_dislink_stage_entry, dislink_stage_entries, process_dislink_stage_entry);
}

static void
mono_gc_register_disappearing_link (MonoObject *obj, void **link, gboolean track, gboolean in_gc)
{
	if (obj)
		*link = HIDE_POINTER (obj, track);
	else
		*link = NULL;

#if 1
	if (in_gc) {
		process_dislink_stage_entry (obj, link);
	} else {
		while (!add_stage_entry (NUM_DISLINK_STAGE_ENTRIES, &next_dislink_stage_entry, dislink_stage_entries, obj, link)) {
			LOCK_GC;
			process_dislink_stage_entries ();
			UNLOCK_GC;
		}
	}
#else
	if (!in_gc)
		LOCK_GC;
	process_dislink_stage_entry (obj, link);
	if (!in_gc)
		UNLOCK_GC;
#endif
}
