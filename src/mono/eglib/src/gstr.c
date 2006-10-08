/*
 * gstr.c: String Utility Functions.
 *
 * Author:
 *   Miguel de Icaza (miguel@novell.com)
 *   Aaron Bockover (abockover@novell.com)
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
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

/* This is not a macro, because I dont want to put _GNU_SOURCE in the glib.h header */
gchar *
g_strndup (const gchar *str, gsize n)
{
	return strndup (str, n);
}

void
g_strfreev (gchar **str_array)
{
	gchar **orig = str_array;
	if (str_array == NULL)
		return;
	while (*str_array != NULL){
		g_free (*str_array);
		str_array++;
	}
	g_free (orig);
}

guint
g_strv_length(gchar **str_array)
{
	gint length = 0;
	g_return_val_if_fail(str_array != NULL, 0);
	for(length = 0; str_array[length] != NULL; length++);
	return length;
}

gboolean
g_str_has_suffix(const gchar *str, const gchar *suffix)
{
	gint str_length;
	gint suffix_length;
	
	g_return_val_if_fail(str != NULL, FALSE);
	g_return_val_if_fail(suffix != NULL, FALSE);

	str_length = strlen(str);
	suffix_length = strlen(suffix);

	return suffix_length <= str_length ?
		strncmp(str + str_length - suffix_length, suffix, suffix_length) == 0 :
		FALSE;
}

gboolean
g_str_has_prefix(const gchar *str, const gchar *prefix)
{
	gint str_length;
	gint prefix_length;
	
	g_return_val_if_fail(str != NULL, FALSE);
	g_return_val_if_fail(prefix != NULL, FALSE);

	str_length = strlen(str);
	prefix_length = strlen(prefix);

	return prefix_length <= str_length ?
		strncmp(str, prefix, prefix_length) == 0 :
		FALSE;
}

gchar *
g_strdup_vprintf (const gchar *format, va_list args)
{
	int n;
	char *ret;
	
	n = vasprintf (&ret, format, args);
	if (n == -1)
		return NULL;

	return ret;
}

gchar *
g_strdup_printf (const gchar *format, ...)
{
	gchar *ret;
	va_list args;
	int n;

	va_start (args, format);
	n = vasprintf (&ret, format, args);
	va_end (args);
	if (n == -1)
		return NULL;

	return ret;
}

const gchar *
g_strerror (gint errnum)
{
	return strerror (errnum);
}

gchar *
g_strconcat (const gchar *first, ...)
{
	g_return_val_if_fail (first != NULL, NULL);
	va_list args;
	int total = 0;
	char *s, *ret;

	total += strlen (first);
	va_start (args, first);
	for (s = va_arg (args, char *); s != NULL; s = va_arg(args, char *)){
		total += strlen (s);
	}
	va_end (args);
	
	ret = g_malloc (total + 1);
	if (ret == NULL)
		return NULL;

	ret [total] = 0;
	strcpy (ret, first);
	va_start (args, first);
	for (s = va_arg (args, char *); s != NULL; s = va_arg(args, char *)){
		strcat (ret, s);
	}
	va_end (args);

	return ret;
}

gchar ** 
g_strsplit (const gchar *string, const gchar *delimiter, gint max_tokens)
{
	gchar *string_c;
	gchar *strtok_save, **vector;
	gchar *token, *token_c;
	gint size = 1;
	gint token_length;

	g_return_val_if_fail(string != NULL, NULL);
	g_return_val_if_fail(delimiter != NULL, NULL);
	g_return_val_if_fail(delimiter[0] != 0, NULL);
	
	token_length = strlen(string);
	string_c = (gchar *)g_malloc(token_length + 1);
	memcpy(string_c, string, token_length);
	string_c[token_length] = 0;
	
	vector = NULL;
	token = (gchar *)strtok_r(string_c, delimiter, &strtok_save);

	while(token != NULL) {
		token_length = strlen(token);
		token_c = (gchar *)g_malloc(token_length + 1);
		memcpy(token_c, token, token_length);
		token_c[token_length] = 0;

		vector = vector == NULL ? 
			(gchar **)g_malloc(2 * sizeof(vector)) :
			(gchar **)g_realloc(vector, (size + 1) * sizeof(vector));
	
		vector[size - 1] = token_c;	
		size++;

		if(max_tokens > 0 && size >= max_tokens) {
			if(size > max_tokens) {
				break;
			}

			token = strtok_save;
		} else {
			token = (gchar *)strtok_r(NULL, delimiter, &strtok_save);
		}
	}

	if(vector != NULL && size > 0) {
		vector[size - 1] = NULL;
	}
	
	g_free(string_c);
	string_c = NULL;

	return vector;
}

gchar *
g_strreverse (gchar *str)
{
	guint len, half;
	gint i;
	gchar c;

	if (str == NULL)
		return NULL;

	len = strlen (str);
	half = len / 2;
	len--;
	for (i = 0; i < half; i++, len--) {
		c = str [i];
		str [i] = str [len];
		str [len] = c;
	}
	return str;
}

gchar *
g_strjoin (const gchar *separator, ...)
{
	va_list args;
	char *res, *s;
	int len, slen;

	if (separator != NULL)
		slen = strlen (separator);
	else
		slen = 0;
	len = 0;
	va_start (args, separator);
	for (s = va_arg (args, char *); s != NULL; s = va_arg (args, char *)){
		len += strlen (s);
		len += slen;
	}
	va_end (args);
	if (len == 0)
		return g_strdup ("");
	
	/* Remove the last separator */
	if (slen > 0 && len > 0)
		len -= slen;
	len++;
	res = g_malloc (len);
	va_start (args, separator);
	s = va_arg (args, char *);
	strcpy (res, s);
	for (s = va_arg (args, char *); s != NULL; s = va_arg (args, char *)){
		if (separator != NULL)
			strcat (res, separator);
		strcat (res, s);
	}
	va_end (args);

	return res;
}

gchar *
g_strchug (gchar *str)
{
	gint len;
	gchar *tmp;

	if (str == NULL)
		return NULL;

	tmp = str;
	while (*tmp && isspace (*tmp)) tmp++;
	if (str != tmp) {
		len = strlen (str) - (tmp - str - 1);
		memmove (str, tmp, len);
	}
	return str;
}

gchar *
g_strchomp (gchar *str)
{
	gchar *tmp;

	if (str == NULL)
		return NULL;

	tmp = str + strlen (str) - 1;
	while (*tmp && isspace (*tmp)) tmp--;
	*(tmp + 1) = '\0';
	return str;
}

gint
g_printf(gchar const *format, ...)
{
	va_list args;
	gint ret;

	va_start(args, format);
	ret = vprintf(format, args);
	va_end(args);

	return ret;
}

gint
g_fprintf(FILE *file, gchar const *format, ...)
{
	va_list args;
	gint ret;

	va_start(args, format);
	ret = vfprintf(file, format, args);
	va_end(args);

	return ret;
}

gint
g_sprintf(gchar *string, gchar const *format, ...)
{
	va_list args;
	gint ret;

	va_start(args, format);
	ret = vsprintf(string, format, args);
	va_end(args);

	return ret;
}

gint
g_snprintf(gchar *string, gulong n, gchar const *format, ...)
{
	va_list args;
	gint ret;
	
	va_start(args, format);
	ret = vsnprintf(string, n, format, args);
	va_end(args);

	return ret;
}

static const char const hx [] = { '0', '1', '2', '3', '4', '5', '6', '7',
				  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

static gboolean
char_needs_encoding (char c)
{
	if (((unsigned char)c) >= 0x80)
		return TRUE;
	
	if ((c >= '@' && c <= 'Z') ||
	    (c >= 'a' && c <= 'z') ||
	    (c >= '&' && c < 0x3b) ||
	    (c == '!') || (c == '$') || (c == '_') || (c == '=') || (c == '~'))
		return FALSE;
	return TRUE;
}

gchar *
g_filename_to_uri (const gchar *filename, const gchar *hostname, GError **error)
{
	int n;
	char *ret, *rp;
	const char *p;
	
	g_return_val_if_fail (filename != NULL, NULL);

	if (hostname != NULL)
		g_warning ("eglib: g_filename_to_uri: hostname not handled");

	if (*filename != '/'){
		if (error != NULL)
			*error = g_error_new (NULL, 2, "Not an absolute filename");
		
		return NULL;
	}
	
	n = strlen ("file://") + 1;
	for (p = filename; *p; p++){
		if (char_needs_encoding (*p))
			n += 3;
		else
			n++;
	}
	ret = g_malloc (n);
	strcpy (ret, "file://");
	for (p = filename, rp = ret + strlen (ret); *p; p++){
		if (char_needs_encoding (*p)){
			*rp++ = '%';
			*rp++ = hx [((unsigned char)(*p)) >> 4];
			*rp++ = hx [((unsigned char)(*p)) & 0xf];
		} else
			*rp++ = *p;
	}
	*rp = 0;
	return ret;
}

static int
decode (char p)
{
	if (p >= '0' && p <= '9')
		return p - '0';
	if (p >= 'A' && p <= 'F')
		return p - 'A';
	if (p >= 'a' && p <= 'f')
		return p - 'a';
	g_assert_not_reached ();
	return 0;
}

gchar *
g_filename_from_uri (const gchar *uri, gchar **hostname, GError **error)
{
	const char *p;
	char *r, *result;
	int flen = 0;
	
	g_return_val_if_fail (uri != NULL, NULL);

	if (hostname != NULL)
		g_warning ("eglib: g_filename_from_uri: hostname not handled");

	if (strncmp (uri, "file:///", 8) != 0){
		if (error != NULL)
			*error = g_error_new (NULL, 2, "URI does not start with the file: scheme");
		return NULL;
	}

	for (p = uri + 8; *p; p++){
		if (*p == '%'){
			if (p [1] && p [2] && isxdigit (p [1]) && isxdigit (p [2])){
				p += 2;
			} else {
				if (error != NULL)
					*error = g_error_new (NULL, 2, "URI contains an invalid escape sequence");
				return NULL;
			}
		} 
		flen++;
	}
	flen++;
	
	result = g_malloc (flen + 1);
	*result = '/';
	result [flen] = 0;

	for (p = uri + 8, r = result + 1; *p; p++){
		if (*p == '%'){
			*r++ = (decode (p [1]) << 4) | decode (p [2]);
			p += 2;
		} else
			*r++ = *p;
		flen++;
	}
	return result;
}

void
g_strdown (gchar *string)
{
	g_return_if_fail (string != NULL);

	while (*string){
		*string = tolower (*string);
	}
}

gchar *
g_ascii_strdown (const gchar *str, gssize len)
{
	char *ret;
	int i;
	
	g_return_val_if_fail  (str != NULL, NULL);

	if (len == -1)
		len = strlen (str);
	
	ret = g_malloc (len + 1);
	for (i = 0; i < len; i++){
		guchar c = (guchar) str [i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		ret [i] = c;
	}
	ret [i] = 0;
	
	return ret;
}


gchar *
g_strdelimit (gchar *string, const gchar *delimiters, gchar new_delimiter)
{
	gchar *ptr;

	g_return_val_if_fail (string != NULL, NULL);

	if (delimiters == NULL)
		delimiters = G_STR_DELIMITERS;

	for (ptr = string; *ptr; ptr++) {
		if (strchr (delimiters, *ptr))
			*ptr = new_delimiter;
	}
	
	return string;
}

#ifndef HAVE_STRLCPY
gsize 
g_strlcpy (gchar *dest, const gchar *src, gsize dest_size)
{
	gchar *d;
	const gchar *s;
	gchar c;
	gsize len;
	
	g_return_val_if_fail (src != NULL, 0);
	g_return_val_if_fail (dest != NULL, 0);

	len = dest_size;
	if (len == 0)
		return 0;

	s = src;
	d = dest;
	while (--len) {
		c = *s++;
		*d++ = c;
		if (c == '\0')
			return (dest_size - len - 1);
	}

	/* len is 0 i we get here */
	*d = '\0';
	/* we need to return the length of src here */
	while (*s++) ; /* instead of a plain strlen, we use 's' */
	return s - src - 1;
}
#endif

static gchar escaped_dflt [256] = {
	1, 1, 1, 1, 1, 1, 1, 1, 'b', 't', 'n', 1, 'f', 'r', 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	0, 0, '"', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

gchar *
g_strescape (const gchar *source, const gchar *exceptions)
{
	gchar escaped [256];
	const gchar *ptr;
	gchar c;
	int op;
	gchar *result;
	gchar *res_ptr;

	g_return_val_if_fail (source != NULL, NULL);

	memcpy (escaped, escaped_dflt, 256);
	if (exceptions != NULL) {
		for (ptr = exceptions; *ptr; ptr++)
			escaped [(int) *ptr] = 0;
	}
	result = g_malloc (strlen (source) * 4 + 1); /* Worst case: everything octal. */
	res_ptr = result;
	for (ptr = source; *ptr; ptr++) {
		c = *ptr;
		op = escaped [(int) c];
		if (op == 0) {
			*res_ptr++ = c;
		} else {
			*res_ptr++ = '\\';
			if (op != 1) {
				*res_ptr++ = op;
			} else {
				*res_ptr++ = '0' + ((c >> 6) & 3);
				*res_ptr++ = '0' + ((c >> 3) & 7);
				*res_ptr++ = '0' + (c & 7);
			}
		}
	}
	*res_ptr = '\0';
	return result;
}

