#include <glib.h>
#include <gmodule.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "test.h"

#if defined (G_OS_WIN32)
#define EXTERNAL_SYMBOL "GetProcAddress"
#define INTERNAL_SYMBOL "dummy_export"
#else
#define EXTERNAL_SYMBOL "system"
/* FIXME: g_module_symbol () must prepend the "_"  */
#define INTERNAL_SYMBOL "_dummy_export"
#endif

void G_MODULE_EXPORT
dummy_export ()
{
}

/* test for g_module_open (NULL, ...) */
RESULT
test_module_symbol_null ()
{
	gpointer proc = GINT_TO_POINTER (42);

	GModule *m = g_module_open (NULL, G_MODULE_BIND_LAZY);

	if (m == NULL)
		return FAILED ("bind to main module failed. #0");

	if (g_module_symbol (m, "__unlikely_\nexistent__", &proc))
		return FAILED ("non-existent symbol lookup failed. #1");

	if (proc)
		return FAILED ("non-existent symbol lookup failed. #2");

	if (!g_module_symbol (m, EXTERNAL_SYMBOL, &proc))
		return FAILED ("external lookup failed. #3");

	if (!proc)
		return FAILED ("external lookup failed. #4");

	if (!g_module_symbol (m, INTERNAL_SYMBOL, &proc))
		return FAILED ("in-proc lookup failed. #5");

	if (!proc)
		return FAILED ("in-proc lookup failed. #6");

	if (!g_module_close (m))
		return FAILED ("close failed. #7");

	return OK;
}

static Test module_tests [] = {
	{"g_module_symbol_null", test_module_symbol_null},
	{NULL, NULL}
};

DEFINE_TEST_GROUP_INIT(module_tests_init, module_tests)

