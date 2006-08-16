#ifndef __GLIB_H
#define __GLIB_H

#include <stdarg.h>
#include <stdlib.h>

/*
 * Macros
 */
#define G_N_ELEMENTS(s)      (sizeof(s) / sizeof ((s) [0]))

#define FALSE                0
#define TRUE                 1

#define G_MAXINT32           0xf7777777
#define G_MININT32           0x80000000

#define GPOINTER_TO_INT(ptr)   ((int)(ptr))
#define GPOINTER_TO_UINT(ptr)  ((uint)(ptr))
#define GINT_TO_POINTER(v)     ((gpointer) (v))
#define GUINT_TO_POINTER(v)    ((gpointer) (v))

/*
 * Allocation
 */
#define g_new(type,size)        ((type *) malloc (sizeof (type) * (size)))
#define g_new0(type,size)       ((type *) calloc (sizeof (type), (size))) 
#define g_free(obj)             free (obj);
#define g_realloc(obj,size)     realloc((obj), (size))
#define g_strdup(x)             strdup(x)
#define g_malloc(x)             malloc(x)
#define g_try_malloc(x)         malloc(x)
#define g_try_realloc(obj,size) realloc((obj),(size))
#define g_malloc0(x)            calloc(1,x)

/*
 * Basic data types
 */
typedef int            gboolean;
typedef unsigned int   guint;
typedef short          gshort;
typedef unsigned short gushort;
typedef long           glong;
typedef unsigned long  gulong;
typedef void *         gpointer;
typedef const void *   gconstpointer;
typedef char           gchar;
typedef unsigned char  guchar;

/*
 * Precondition macros
 */
#define g_return_if_fail(x)  do { if (!(x)) { printf ("%s:%d: assertion %s failed", __FILE__, __LINE__, #x); return; } } while (0) ;
#define g_return_val_if_fail(x,e)  do { if (!(x)) { printf ("%s:%d: assertion %s failed", __FILE__, __LINE__, #x); return (e); } } while (0) ;

/*
 * Hashtables
 */
typedef struct _GHashTable GHashTable;
typedef void     (*GHFunc)         (gpointer key, gpointer value, gpointer user_data);
typedef gboolean (*GHRFunc)        (gpointer key, gpointer value, gpointer user_data);
typedef void     (*GDestroyNotify) (gpointer data);
typedef guint    (*GHashFunc)      (gconstpointer key);
typedef gboolean (*GEqualFunc)     (gconstpointer a, gconstpointer b);

GHashTable     *g_hash_table_new             (GHashFunc hash_func, GEqualFunc key_equal_func);
void            g_hash_table_insert_replace  (GHashTable *hash, gpointer key, gpointer value, gboolean replace);
guint           g_hash_table_size            (GHashTable *hash);
gpointer        g_hash_table_lookup          (GHashTable *hash, gconstpointer key);
gboolean        g_hash_table_lookup_extended (GHashTable *hash, gconstpointer key, gpointer *orig_key, gpointer *value);
void            g_hash_table_foreach         (GHashTable *hash, GHFunc func, gpointer user_data);
gpointer        g_hash_table_find            (GHashTable *hash, GHRFunc predicate, gpointer user_data);
gboolean        g_hash_table_remove          (GHashTable *hash, gconstpointer key);
guint           g_hash_table_foreach_remove  (GHashTable *hash, GHRFunc func, gpointer user_data);
void            g_hash_table_destroy         (GHashTable *hash);

#define g_hash_table_insert(h,k,v)    g_hash_table_insert_replace ((h),(k),(v),FALSE)
#define g_hash_table_replace(h,k,v)   g_hash_table_insert_replace ((h),(k),(v),TRUE)

gboolean g_direct_equal (gconstpointer v1, gconstpointer v2);
guint    g_direct_hash  (gconstpointer v1);
gboolean g_int_equal    (gconstpointer v1, gconstpointer v2);
guint    g_int_hash     (gconstpointer v1);
gboolean g_str_equal    (gconstpointer v1, gconstpointer v2);
guint    g_str_hash     (gconstpointer v1);

#define  g_assert(x)     do { fprintf (stderr, "* Assertion at %s:%d, condition `%s' not met\n", __FILE__, __LINE__, #x); abort (); } while (0)
#define  g_assert_not_reached() do { fprintf (stderr, "* This line should not be reached at %s:%d\n", __FILE__, __LINE__); } while (0)

/*
 * Strings
 */
gchar   *g_strdup_printf (const gchar *format, ...);

/*
 * Messages
 */
#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN ((gchar*) 0)
#endif

typedef enum {
	G_LOG_FLAG_RECURSION          = 1 << 0,
	G_LOG_FLAG_FATAL              = 1 << 1,
	
	G_LOG_LEVEL_ERROR             = 1 << 2,
	G_LOG_LEVEL_CRITICAL          = 1 << 3,
	G_LOG_LEVEL_WARNING           = 1 << 4,
	G_LOG_LEVEL_MESSAGE           = 1 << 5,
	G_LOG_LEVEL_INFO              = 1 << 6,
	G_LOG_LEVEL_DEBUG             = 1 << 7,
	
	G_LOG_LEVEL_MASK              = ~(G_LOG_FLAG_RECURSION | G_LOG_FLAG_FATAL)
} GLogLevelFlags;

void           g_print                (const gchar *format, ...);
GLogLevelFlags g_log_set_always_fatal (GLogLevelFlags fatal_mask);
GLogLevelFlags g_log_set_fatal_mask   (const gchar *log_domain, GLogLevelFlags fatal_mask);
void           g_logv                 (const gchar *log_domain, GLogLevelFlags log_level, const gchar *format, va_list args);
void           g_log                  (const gchar *log_domain, GLogLevelFlags log_level, const gchar *format, ...);

#define g_error(format...)    g_log (G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, format)
#define g_critical(format...) g_log (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, format)
#define g_warning(format...)  g_log (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, format)
#define g_message(format...)  g_log (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, format)
#define g_debug(format...)    g_log (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, format)
	
#endif
