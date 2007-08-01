/*
 * io-portability.c:  Optional filename mangling to try to cope with
 *			badly-written non-portable windows apps
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * Copyright (c) 2006 Novell, Inc.
 */

#include <config.h>
#include <glib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <utime.h>
#include <sys/stat.h>

#include <mono/io-layer/mono-mutex.h>
#include <mono/io-layer/error.h>
#include <mono/io-layer/wapi_glob.h>
#include <mono/io-layer/io-portability.h>

#undef DEBUG

enum {
	PORTABILITY_NONE	= 0x00,
	PORTABILITY_UNKNOWN	= 0x01,
	PORTABILITY_DRIVE	= 0x02,
	PORTABILITY_CASE	= 0x04,
};

static mono_once_t options_once = MONO_ONCE_INIT;
static int portability_helpers = PORTABILITY_UNKNOWN;

static void options_init (void)
{
	const gchar *env;
	
	portability_helpers = PORTABILITY_NONE;
	
	env = g_getenv ("MONO_IOMAP");
	if (env != NULL) {
		/* parse the environment setting and set up some vars
		 * here
		 */
		gchar **options = g_strsplit (env, ":", 0);
		int i;
		
		if (options == NULL) {
			/* This shouldn't happen */
			return;
		}
		
		for (i = 0; options[i] != NULL; i++) {
#ifdef DEBUG
			g_message ("%s: Setting option [%s]", __func__,
				   options[i]);
#endif
			if (!strncasecmp (options[i], "drive", 5)) {
				portability_helpers |= PORTABILITY_DRIVE;
			} else if (!strncasecmp (options[i], "case", 4)) {
				portability_helpers |= PORTABILITY_CASE;
			} else if (!strncasecmp (options[i], "all", 3)) {
				portability_helpers |= (PORTABILITY_DRIVE |
							PORTABILITY_CASE);
			}
		}
	}
}

/* Returns newly allocated string, or NULL on failure */
static gchar *find_in_dir (DIR *current, const gchar *name)
{
	struct dirent *entry;

#ifdef DEBUG
	g_message ("%s: looking for [%s]\n", __func__, name);
#endif
	
	while((entry = readdir (current)) != NULL) {
#ifdef DEBUGX
		g_message ("%s: found [%s]\n", __func__, entry->d_name);
#endif
		
		if (!g_ascii_strcasecmp (name, entry->d_name)) {
			char *ret;
			
#ifdef DEBUG
			g_message ("%s: matched [%s] to [%s]\n", __func__,
				   entry->d_name, name);
#endif

			ret = g_strdup (entry->d_name);
			closedir (current);
			return ret;
		}
	}
	
#ifdef DEBUG
	g_message ("%s: returning NULL\n", __func__);
#endif
	
	closedir (current);
	
	return(NULL);
}

/* Returns newly-allocated string or NULL on failure */
static gchar *find_file (const gchar *pathname, gboolean last_exists)
{
	gchar *new_pathname, **components, **new_components;
	int num_components = 0, component = 0;
	DIR *scanning = NULL;
	
	mono_once (&options_once, options_init);

	if (portability_helpers == PORTABILITY_NONE) {
		return(NULL);
	}

	new_pathname = g_strdup (pathname);
	
#ifdef DEBUG
	g_message ("%s: Finding [%s] last_exists: %s\n", __func__, pathname,
		   last_exists?"TRUE":"FALSE");
#endif
	
	if (last_exists &&
	    access (new_pathname, F_OK) == 0) {
#ifdef DEBUG
		g_message ("%s: Found it without doing anything\n", __func__);
#endif
		return(new_pathname);
	}
	
	/* First turn '\' into '/' and strip any drive letters */
	g_strdelimit (new_pathname, "\\", '/');

#ifdef DEBUG
	g_message ("%s: Fixed slashes, now have [%s]\n", __func__,
		   new_pathname);
#endif
	
	if (portability_helpers & PORTABILITY_DRIVE &&
	    g_ascii_isalpha (new_pathname[0]) &&
	    (new_pathname[1] == ':')) {
		int len = strlen (new_pathname);
		
		g_memmove (new_pathname, new_pathname+2, len - 2);
		new_pathname[len - 2] = '\0';
		
#ifdef DEBUG
		g_message ("%s: Stripped drive letter, now looking for [%s]\n",
			   __func__, new_pathname);
#endif
	}
	
	if (last_exists &&
	    access (new_pathname, F_OK) == 0) {
#ifdef DEBUG
		g_message ("%s: Found it\n", __func__);
#endif
		
		return(new_pathname);
	}

	/* OK, have to work harder.  Take each path component in turn
	 * and do a case-insensitive directory scan for it
	 */

	if (!(portability_helpers & PORTABILITY_CASE)) {
		g_free (new_pathname);
		return(NULL);
	}

	components = g_strsplit (new_pathname, "/", 0);
	if (components == NULL) {
		/* This shouldn't happen */
		g_free (new_pathname);
		return(NULL);
	}
	
	while(components[num_components] != NULL) {
		num_components++;
	}
	g_free (new_pathname);
	
	if (num_components == 0){
		return NULL;
	}
	

	new_components = (gchar **)g_new0 (gchar **, num_components + 1);

	if (num_components > 1) {
		if (strcmp (components[0], "") == 0) {
			/* first component blank, so start at / */
			scanning = opendir ("/");
			if (scanning == NULL) {
#ifdef DEBUG
				g_message ("%s: opendir 1 error: %s", __func__,
					   g_strerror (errno));
#endif
				g_strfreev (new_components);
				g_strfreev (components);
				return(NULL);
			}
		
			new_components[component++] = g_strdup ("");
		} else {
			DIR *current;
			gchar *entry;
		
			current = opendir (".");
			if (current == NULL) {
#ifdef DEBUG
				g_message ("%s: opendir 2 error: %s", __func__,
					   g_strerror (errno));
#endif
				g_strfreev (new_components);
				g_strfreev (components);
				return(NULL);
			}
		
			entry = find_in_dir (current, components[0]);
			if (entry == NULL) {
				g_strfreev (new_components);
				g_strfreev (components);
				return(NULL);
			}
		
			scanning = opendir (entry);
			if (scanning == NULL) {
#ifdef DEBUG
				g_message ("%s: opendir 3 error: %s", __func__,
					   g_strerror (errno));
#endif
				g_free (entry);
				g_strfreev (new_components);
				g_strfreev (components);
				return(NULL);
			}
		
			new_components[component++] = entry;
		}
	} else {
		if (last_exists) {
			if (strcmp (components[0], "") == 0) {
				/* First and only component blank */
				new_components[component++] = g_strdup ("");
			} else {
				DIR *current;
				gchar *entry;
				
				current = opendir (".");
				if (current == NULL) {
#ifdef DEBUG
					g_message ("%s: opendir 4 error: %s",
						   __func__,
						   g_strerror (errno));
#endif
					g_strfreev (new_components);
					g_strfreev (components);
					return(NULL);
				}
				
				entry = find_in_dir (current, components[0]);
				if (entry == NULL) {
					g_strfreev (new_components);
					g_strfreev (components);
					return(NULL);
				}
				
				new_components[component++] = entry;
			}
		} else {
				new_components[component++] = g_strdup (components[0]);
		}
	}

#ifdef DEBUG
	g_message ("%s: Got first entry: [%s]\n", __func__, new_components[0]);
#endif

	g_assert (component == 1);
	
	for(; component < num_components; component++) {
		gchar *entry;
		gchar *path_so_far;
		
		if (!last_exists &&
		    component == num_components -1) {
			entry = g_strdup (components[component]);
			closedir (scanning);
		} else {
			entry = find_in_dir (scanning, components[component]);
			if (entry == NULL) {
				g_strfreev (new_components);
				g_strfreev (components);
				return(NULL);
			}
		}
		
		new_components[component] = entry;
		
		if (component < num_components -1) {
			path_so_far = g_strjoinv ("/", new_components);

			scanning = opendir (path_so_far);
			g_free (path_so_far);
			if (scanning == NULL) {
				g_strfreev (new_components);
				g_strfreev (components);
				return(NULL);
			}
		}
	}
	
	g_strfreev (components);

	new_pathname = g_strjoinv ("/", new_components);

#ifdef DEBUG
	g_message ("%s: pathname [%s] became [%s]\n", __func__, pathname,
		   new_pathname);
#endif
	
	g_strfreev (new_components);

	if ((last_exists &&
	     access (new_pathname, F_OK) == 0) ||
	    (!last_exists)) {
		return(new_pathname);
	}
	
	g_free (new_pathname);
	return(NULL);
}

int _wapi_open (const char *pathname, int flags, mode_t mode)
{
	int fd;
	gchar *located_filename;
	
	if (flags & O_CREAT) {
		located_filename = find_file (pathname, FALSE);
		if (located_filename == NULL) {
			fd = open (pathname, flags, mode);
		} else {
			fd = open (located_filename, flags, mode);
			g_free (located_filename);
		}
	} else {
		fd = open (pathname, flags, mode);
		if (fd == -1 &&
		    (errno == ENOENT || errno == ENOTDIR) &&
		    portability_helpers > 0) {
			int saved_errno = errno;
			located_filename = find_file (pathname, TRUE);
			
			if (located_filename == NULL) {
				errno = saved_errno;
				return (-1);
			}
			
			fd = open (located_filename, flags, mode);
			g_free (located_filename);
		}
	}
	
	
	return(fd);
}

int _wapi_access (const char *pathname, int mode)
{
	int ret;
	
	ret = access (pathname, mode);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (pathname, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = access (located_filename, mode);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_chmod (const char *pathname, mode_t mode)
{
	int ret;
	
	ret = chmod (pathname, mode);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (pathname, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = chmod (located_filename, mode);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_utime (const char *filename, const struct utimbuf *buf)
{
	int ret;
	
	ret = utime (filename, buf);
	if (ret == -1 &&
	    errno == ENOENT &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (filename, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = utime (located_filename, buf);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_unlink (const char *pathname)
{
	int ret;
	
	ret = unlink (pathname);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR || errno == EISDIR) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (pathname, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = unlink (located_filename);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_rename (const char *oldpath, const char *newpath)
{
	int ret;
	gchar *located_newpath = find_file (newpath, FALSE);
	
	if (located_newpath == NULL) {
		ret = rename (oldpath, newpath);
	} else {
		ret = rename (oldpath, located_newpath);
	
		if (ret == -1 &&
		    (errno == EISDIR || errno == ENAMETOOLONG ||
		     errno == ENOENT || errno == ENOTDIR || errno == EXDEV) &&
		    portability_helpers > 0) {
			int saved_errno = errno;
			gchar *located_oldpath = find_file (oldpath, TRUE);
			
			if (located_oldpath == NULL) {
				g_free (located_oldpath);
				g_free (located_newpath);
			
				errno = saved_errno;
				return(-1);
			}
			
			ret = rename (located_oldpath, located_newpath);
			g_free (located_oldpath);
		}
		g_free (located_newpath);
	}
	
	return(ret);
}

int _wapi_stat (const char *path, struct stat *buf)
{
	int ret;
	
	ret = stat (path, buf);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (path, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = stat (located_filename, buf);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_lstat (const char *path, struct stat *buf)
{
	int ret;
	
	ret = lstat (path, buf);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (path, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = lstat (located_filename, buf);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_mkdir (const char *pathname, mode_t mode)
{
	int ret;
	gchar *located_filename = find_file (pathname, FALSE);
	
	if (located_filename == NULL) {
		ret = mkdir (pathname, mode);
	} else {
		ret = mkdir (located_filename, mode);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_rmdir (const char *pathname)
{
	int ret;
	
	ret = rmdir (pathname);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (pathname, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = rmdir (located_filename);
		g_free (located_filename);
	}
	
	return(ret);
}

int _wapi_chdir (const char *path)
{
	int ret;
	
	ret = chdir (path);
	if (ret == -1 &&
	    (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) &&
	    portability_helpers > 0) {
		int saved_errno = errno;
		gchar *located_filename = find_file (path, TRUE);
		
		if (located_filename == NULL) {
			errno = saved_errno;
			return(-1);
		}
		
		ret = chdir (located_filename);
		g_free (located_filename);
	}
	
	return(ret);
}

gchar *_wapi_basename (const gchar *filename)
{
	gchar *new_filename = g_strdup (filename), *ret;

	mono_once (&options_once, options_init);
	
	g_strdelimit (new_filename, "\\", '/');

	if (portability_helpers & PORTABILITY_DRIVE &&
	    g_ascii_isalpha (new_filename[0]) &&
	    (new_filename[1] == ':')) {
		int len = strlen (new_filename);
		
		g_memmove (new_filename, new_filename + 2, len - 2);
		new_filename[len - 2] = '\0';
	}
	
	ret = g_path_get_basename (new_filename);
	g_free (new_filename);
	
	return(ret);
}

gchar *_wapi_dirname (const gchar *filename)
{
	gchar *new_filename = g_strdup (filename), *ret;

	mono_once (&options_once, options_init);
	
	g_strdelimit (new_filename, "\\", '/');

	if (portability_helpers & PORTABILITY_DRIVE &&
	    g_ascii_isalpha (new_filename[0]) &&
	    (new_filename[1] == ':')) {
		int len = strlen (new_filename);
		
		g_memmove (new_filename, new_filename + 2, len - 2);
		new_filename[len - 2] = '\0';
	}
	
	ret = g_path_get_dirname (new_filename);
	g_free (new_filename);
	
	return(ret);
}

GDir *_wapi_g_dir_open (const gchar *path, guint flags, GError **error)
{
	GDir *ret;
	
	ret = g_dir_open (path, flags, error);
	if (ret == NULL &&
	    ((*error)->code == G_FILE_ERROR_NOENT ||
	     (*error)->code == G_FILE_ERROR_NOTDIR ||
	     (*error)->code == G_FILE_ERROR_NAMETOOLONG) &&
	    portability_helpers > 0) {
		gchar *located_filename = find_file (path, TRUE);
		GError *tmp_error = NULL;
		
		if (located_filename == NULL) {
			return(NULL);
		}
		
		ret = g_dir_open (located_filename, flags, &tmp_error);
		g_free (located_filename);
		if (tmp_error == NULL) {
			g_clear_error (error);
		}
	}
	
	return(ret);
}


static gint
file_compare (gconstpointer a, gconstpointer b)
{
	gchar *astr = *(gchar **) a;
	gchar *bstr = *(gchar **) b;

	return strcmp (astr, bstr);
}

static gint
get_errno_from_g_file_error (gint error)
{
	switch (error) {
#ifdef EACCESS
	case G_FILE_ERROR_ACCES:
		error = EACCES;
		break;
#endif
#ifdef ENAMETOOLONG
	case G_FILE_ERROR_NAMETOOLONG:
		error = ENAMETOOLONG;
		break;
#endif
#ifdef ENOENT
	case G_FILE_ERROR_NOENT:
		error = ENOENT;
		break;
#endif
#ifdef ENOTDIR
	case G_FILE_ERROR_NOTDIR:
		error = ENOTDIR;
		break;
#endif
#ifdef ENXIO
	case G_FILE_ERROR_NXIO:
		error = ENXIO;
		break;
#endif
#ifdef ENODEV
	case G_FILE_ERROR_NODEV:
		error = ENODEV;
		break;
#endif
#ifdef EROFS
	case G_FILE_ERROR_ROFS:
		error = EROFS;
		break;
#endif
#ifdef ETXTBSY
	case G_FILE_ERROR_TXTBSY:
		error = ETXTBSY;
		break;
#endif
#ifdef EFAULT
	case G_FILE_ERROR_FAULT:
		error = EFAULT;
		break;
#endif
#ifdef ELOOP
	case G_FILE_ERROR_LOOP:
		error = ELOOP;
		break;
#endif
#ifdef ENOSPC
	case G_FILE_ERROR_NOSPC:
		error = ENOSPC;
		break;
#endif
#ifdef ENOMEM
	case G_FILE_ERROR_NOMEM:
		error = ENOMEM;
		break;
#endif
#ifdef EMFILE
	case G_FILE_ERROR_MFILE:
		error = EMFILE;
		break;
#endif
#ifdef ENFILE
	case G_FILE_ERROR_NFILE:
		error = ENFILE;
		break;
#endif
#ifdef EBADF
	case G_FILE_ERROR_BADF:
		error = EBADF;
		break;
#endif
#ifdef EINVAL
	case G_FILE_ERROR_INVAL:
		error = EINVAL;
		break;
#endif
#ifdef EPIPE
	case G_FILE_ERROR_PIPE:
		error = EPIPE;
		break;
#endif
#ifdef EAGAIN
	case G_FILE_ERROR_AGAIN:
		error = EAGAIN;
		break;
#endif
#ifdef EINTR
	case G_FILE_ERROR_INTR:
		error = EINTR;
		break;
#endif
#ifdef EWIO
	case G_FILE_ERROR_IO:
		error = EIO;
		break;
#endif
#ifdef EPERM
	case G_FILE_ERROR_PERM:
		error = EPERM;
		break;
#endif
	case G_FILE_ERROR_FAILED:
		error = ERROR_INVALID_PARAMETER;
		break;
	}

	return error;
}

/* scandir using glib */
gint _wapi_io_scandir (const gchar *dirname, const gchar *pattern,
		       gchar ***namelist)
{
	GError *error = NULL;
	GDir *dir;
	GPtrArray *names;
	gint result;
	wapi_glob_t glob_buf;
	int flags = 0, i;
	
	dir = _wapi_g_dir_open (dirname, 0, &error);
	if (dir == NULL) {
		/* g_dir_open returns ENOENT on directories on which we don't
		 * have read/x permission */
		gint errnum = get_errno_from_g_file_error (error->code);
		g_error_free (error);
		if (errnum == ENOENT &&
		    !_wapi_access (dirname, F_OK) &&
		    _wapi_access (dirname, R_OK|X_OK)) {
			errnum = EACCES;
		}

		errno = errnum;
		return -1;
	}

	if (portability_helpers & PORTABILITY_CASE) {
		flags = WAPI_GLOB_IGNORECASE;
	}
	
	result = _wapi_glob (dir, pattern, flags, &glob_buf);
	if (g_str_has_suffix (pattern, ".*")) {
		/* Special-case the patterns ending in '.*', as
		 * windows also matches entries with no extension with
		 * this pattern.
		 * 
		 * TODO: should this be a MONO_IOMAP option?
		 */
		gchar *pattern2 = g_strndup (pattern, strlen (pattern) - 2);
		
		g_dir_rewind (dir);
		result = _wapi_glob (dir, pattern2, flags | WAPI_GLOB_APPEND | WAPI_GLOB_UNIQUE, &glob_buf);

		g_free (pattern2);
	}
	
	g_dir_close (dir);
	if (glob_buf.gl_pathc == 0) {
		return(0);
	} else if (result != 0) {
		return(-1);
	}
	
	names = g_ptr_array_new ();
	for (i = 0; i < glob_buf.gl_pathc; i++) {
		g_ptr_array_add (names, g_strdup (glob_buf.gl_pathv[i]));
	}

	_wapi_globfree (&glob_buf);

	result = names->len;
	if (result > 0) {
		g_ptr_array_sort (names, file_compare);
		g_ptr_array_set_size (names, result + 1);

		*namelist = (gchar **) g_ptr_array_free (names, FALSE);
	} else {
		g_ptr_array_free (names, TRUE);
	}

	return result;
}
