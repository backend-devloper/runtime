/*
 * EGLib Unit Group/Test Runners
 *
 * Author:
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <glib.h>

#include "test.h"

static gchar *last_result = NULL;

gboolean 
run_test(Test *test, gchar **result_out)
{
	gchar *result; 

	if((result = test->handler()) == NULL) {
		*result_out = NULL;
		return TRUE;
	} else {
		*result_out = result;	
		return FALSE;
	}
}

gboolean
run_group(Group *group, gint iterations, gboolean quiet, 
	gboolean time, gchar *tests_to_run_s)
{
	Test *tests = group->handler();
	gint i, j, passed = 0, total = 0;
	gdouble start_time_group, start_time_test;
	gchar **tests_to_run = NULL;

	if(!quiet) {
		if(iterations > 1) {
			printf("[%s] (%dx)\n", group->name, iterations);
		} else {
			printf("[%s]\n", group->name);
		}
	}

	if(tests_to_run_s != NULL) {
		tests_to_run = eg_strsplit(tests_to_run_s, ",", -1);
	}

	start_time_group = get_timestamp();

	for(i = 0; tests[i].name != NULL; i++) {
		gchar *result;
		gboolean iter_pass, run;
	
		if(tests_to_run != NULL) {
			gint j;
			run = FALSE;
			for(j = 0; tests_to_run[j] != NULL; j++) {
				if(strcmp(tests_to_run[j], tests[i].name) == 0) {
					run = TRUE;
					break;
				}
			}
		} else {
			run = TRUE;
		}

		if(!run) {
			continue;
		}
	
		total++;
	
		if(!quiet) {
			printf("  %s: ", tests[i].name);
		}

		start_time_test = get_timestamp();
		
		for(j = 0; j < iterations; j++) {
			iter_pass = run_test(&(tests[i]), &result);
			if(!iter_pass) {
				break;
			}
		}

		if(iter_pass) {
			passed++;
			if(!quiet) {
				if(time) {
					printf("OK (%g)\n", get_timestamp() - start_time_test);
				} else {
					printf("OK\n");
				}
			}
		} else  {			
			if(!quiet) {
				printf("FAILED (%s)\n", result);
			}
			
			if(last_result == result) {
				last_result = NULL;
				g_free(result);
			}
		}
	}

	if(!quiet) {
		gdouble pass_percentage = ((gdouble)passed / (gdouble)total) * 100.0;
		if(time) {
			printf("  %d / %d (%g%%, %g)\n", passed, total,
				pass_percentage, get_timestamp() - start_time_group);
		} else {
			printf("  %d / %d (%g%%)\n", passed, total, pass_percentage);
		}
	}

	if(tests_to_run != NULL) {
		eg_strfreev(tests_to_run);
	}

	return passed == total;
}

RESULT
FAILED(const gchar *format, ...)
{
	gchar *ret;
	va_list args;
	gint n;

	va_start(args, format);
	n = vasprintf(&ret, format, args);
	va_end(args);

	if(n == -1) {
		last_result = NULL;
		return NULL;
	}

	last_result = ret;
	return ret;
}

gdouble
get_timestamp()
{
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return (gdouble)tp.tv_sec + (1.e-6) * tp.tv_usec;
}

/* 
 * Duplicating code here from EGlib to avoid g_strsplit skew between
 * EGLib and GLib
 */
 
gchar ** 
eg_strsplit (const gchar *string, const gchar *delimiter, gint max_tokens)
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

void
eg_strfreev (gchar **str_array)
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


