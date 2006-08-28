/*
 * Directory utility functions.
 *
 * Author:
 *   Gonzalo Paniagua Javier (gonzalo@novell.com)
 *
 * (C) 2006 Novell, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

struct _GDir {
#ifdef G_OS_WIN32
#else
	DIR *dir;
#endif
};

GDir *
g_dir_open (const gchar *path, guint flags, GError **error)
{
#ifdef G_OS_WIN32
	return NULL;
#else
	GDir *dir;

	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	(void) flags; /* this is not used */
	dir = g_new (GDir, 1);
	dir->dir = opendir (path);
	if (dir->dir == NULL) {
		if (error) {
			gint err = errno;
			*error = g_error_new (G_LOG_DOMAIN, g_file_error_from_errno (err), strerror (err));
		}
		g_free (dir);
		return NULL;
	}
	return dir;
#endif
}

const gchar *
g_dir_read_name (GDir *dir)
{
#ifdef G_OS_WIN32
	return NULL;
#else
	struct dirent *entry;

	g_return_val_if_fail (dir != NULL && dir->dir != NULL, NULL);
	entry = readdir (dir->dir);
	if (entry == NULL)
		return NULL;

	return entry->d_name;
}

void
g_dir_rewind (GDir *dir)
{
#ifdef G_OS_WIN32
#else
	g_return_if_fail (dir != NULL && dir->dir != NULL);
	rewinddir (dir->dir);
}

void
g_dir_close (GDir *dir)
{
#ifdef G_OS_WIN32
#else
	g_return_if_fail (dir != NULL && dir->dir != NULL);
	closedir (dir->dir);
	dir->dir = NULL;
	g_free (dir);
#endif
}

