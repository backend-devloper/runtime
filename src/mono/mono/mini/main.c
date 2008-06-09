#include <config.h>
#include "mini.h"

#ifdef PLATFORM_WIN32

int
main ()
{
	int argc;
	gunichar2** argvw;
	gchar** argv;
	int i;

	argvw = CommandLineToArgvW (GetCommandLine (), &argc);
	argv = g_new0 (gchar*, argc + 1);
	for (i = 0; i < argc; i++)
		argv [i] = g_utf16_to_utf8 (argvw [i], -1, NULL, NULL, NULL);
	argv [argc] = NULL;

	LocalFree (argvw);

	return mono_main (argc, argv);
}

#else

int
main (int argc, char* argv[])
{
	return mono_main (argc, argv);
}

#endif
