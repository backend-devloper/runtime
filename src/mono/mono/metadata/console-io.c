/*
 * console-io.c: ConsoleDriver internal calls
 *
 * Author:
 *	Gonzalo Paniagua Javier (gonzalo@ximian.com)
 *
 * Copyright (C) 2005 Novell, Inc. (http://www.novell.com)
 */

#include <config.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/threadpool.h>
/* On solaris, curses.h must come before both termios.h and term.h */
#ifdef HAVE_CURSES_H
#include <curses.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_TERM_H
#include <term.h>
#endif
/* Needed for FIONREAD under solaris */
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#ifndef PLATFORM_WIN32
#ifndef TIOCGWINSZ
#include <sys/ioctl.h>
#endif
#endif

#include <mono/metadata/console-io.h>
#include <mono/metadata/exception.h>

static gboolean setup_finished;
static gboolean atexit_called;
static gchar *teardown_str;

#ifdef PLATFORM_WIN32
MonoBoolean
ves_icall_System_ConsoleDriver_Isatty (HANDLE handle)
{
	MONO_ARCH_SAVE_REGS;

	return (GetFileType (handle) == FILE_TYPE_CHAR);
}

MonoBoolean
ves_icall_System_ConsoleDriver_SetEcho (MonoBoolean want_echo)
{
	return FALSE;
}

MonoBoolean
ves_icall_System_ConsoleDriver_SetBreak (MonoBoolean want_break)
{
	return FALSE;
}

gint32
ves_icall_System_ConsoleDriver_InternalKeyAvailable (gint32 timeout)
{
	return FALSE;
}

MonoBoolean
ves_icall_System_ConsoleDriver_TtySetup (MonoString *teardown, char *verase, char *vsusp, char *intr)
{
	return FALSE;
}

MonoBoolean
ves_icall_System_ConsoleDriver_GetTtySize (HANDLE handle, gint32 *width, gint32 *height)
{
	return FALSE;
}

#else
static struct termios initial_attr;

MonoBoolean
ves_icall_System_ConsoleDriver_Isatty (HANDLE handle)
{
	MONO_ARCH_SAVE_REGS;

	return isatty (GPOINTER_TO_INT (handle));
}

static MonoBoolean
set_property (gint property, gboolean value)
{
	struct termios attr;
	gboolean callset = FALSE;
	gboolean check;
	
	MONO_ARCH_SAVE_REGS;

	if (tcgetattr (STDIN_FILENO, &attr) == -1)
		return FALSE;

	check = (attr.c_lflag & property) != 0;
	if ((value || check) && !(value && check)) {
		callset = TRUE;
		if (value)
			attr.c_lflag |= property;
		else
			attr.c_lflag &= ~property;
	}

	if (!callset)
		return TRUE;

	if (tcsetattr (STDIN_FILENO, TCSANOW, &attr) == -1)
		return FALSE;

	return TRUE;
}

MonoBoolean
ves_icall_System_ConsoleDriver_SetEcho (MonoBoolean want_echo)
{
	return set_property (ECHO, want_echo);
}

MonoBoolean
ves_icall_System_ConsoleDriver_SetBreak (MonoBoolean want_break)
{
	return set_property (IGNBRK, !want_break);
}

gint32
ves_icall_System_ConsoleDriver_InternalKeyAvailable (gint32 timeout)
{
	fd_set rfds;
	struct timeval tv;
	struct timeval *tvptr;
	div_t divvy;
	int ret, nbytes;

	MONO_ARCH_SAVE_REGS;

	do {
		FD_ZERO (&rfds);
		FD_SET (STDIN_FILENO, &rfds);
		if (timeout >= 0) {
			divvy = div (timeout, 1000);
			tv.tv_sec = divvy.quot;
			tv.tv_usec = divvy.rem;
			tvptr = &tv;
		} else {
			tvptr = NULL;
		}
		ret = select (STDIN_FILENO + 1, &rfds, NULL, NULL, tvptr);
	} while (ret == -1 && errno == EINTR);

	if (ret > 0) {
		nbytes = 0;
		ret = ioctl (STDIN_FILENO, FIONREAD, &nbytes);
		if (ret >= 0)
			ret = nbytes;
	}

	return (ret > 0) ? ret : 0;
}

static void
tty_teardown (void)
{
	MONO_ARCH_SAVE_REGS;

	if (!setup_finished)
		return;

	if (teardown_str != NULL) {
		write (STDOUT_FILENO, teardown_str, strlen (teardown_str));
		g_free (teardown_str);
		teardown_str = NULL;
	}

	tcflush (STDIN_FILENO, TCIFLUSH);
	tcsetattr (STDIN_FILENO, TCSANOW, &initial_attr);
	set_property (ECHO, TRUE);
	setup_finished = FALSE;
}

static void
do_console_cancel_event (void)
{
	static MonoClassField *cancel_handler_field;
	MonoDomain *domain = mono_domain_get ();
	MonoClass *klass;
	MonoDelegate *load_value;
	MonoMethod *method;
	MonoMethodMessage *msg;
	MonoMethod *im;

	if (!domain->domain)
		return;

	klass = mono_class_from_name (mono_defaults.corlib, "System", "Console");
	if (klass == NULL)
		return;

	if (cancel_handler_field == NULL) {
		cancel_handler_field = mono_class_get_field_from_name (klass, "cancel_handler");
		g_assert (cancel_handler_field);
	}

	mono_field_static_get_value (mono_class_vtable (domain, klass), cancel_handler_field, &load_value);
	if (load_value == NULL)
		return;

	klass = load_value->object.vtable->klass;
	method = mono_class_get_method_from_name (klass, "BeginInvoke", -1);
	g_assert (method != NULL);
	im = mono_get_delegate_invoke (method->klass);
	msg = mono_method_call_message_new (method, NULL, im, NULL, NULL);
	mono_thread_pool_add ((MonoObject *) load_value, msg, NULL, NULL);
}

static gboolean in_sigint;
static void
sigint_handler (int signo)
{
	MONO_ARCH_SAVE_REGS;

	if (in_sigint)
		return;

	in_sigint = TRUE;
	do_console_cancel_event ();
	in_sigint = FALSE;
}

MonoBoolean
ves_icall_System_ConsoleDriver_TtySetup (MonoString *teardown, char *verase, char *vsusp, char*intr)
{
	struct termios attr;
	
	MONO_ARCH_SAVE_REGS;


	*verase = '\0';
	*vsusp = '\0';
	*intr = '\0';
	if (tcgetattr (STDIN_FILENO, &initial_attr) == -1)
		return FALSE;

	/* TODO: handle SIGTSTP - Ctrl-Z */
	attr = initial_attr;
	attr.c_lflag &= ~ICANON;
	attr.c_cc [VMIN] = 1;
	attr.c_cc [VTIME] = 0;
	if (tcsetattr (STDIN_FILENO, TCSANOW, &attr) == -1)
		return FALSE;

	*verase = initial_attr.c_cc [VERASE];
	*vsusp = initial_attr.c_cc [VSUSP];
	*intr = initial_attr.c_cc [VINTR];
	/* If initialized from another appdomain... */
	if (setup_finished)
		return TRUE;

	signal (SIGINT, sigint_handler);
	setup_finished = TRUE;
	if (!atexit_called) {
		if (teardown != NULL)
			teardown_str = mono_string_to_utf8 (teardown);

		atexit (tty_teardown);
	}

	return TRUE;
}

MonoBoolean
ves_icall_System_ConsoleDriver_GetTtySize (HANDLE handle, gint32 *width, gint32 *height)
{
#ifdef TIOCGWINSZ
	struct winsize ws;
	int res;

	MONO_ARCH_SAVE_REGS;

	res = ioctl (GPOINTER_TO_INT (handle), TIOCGWINSZ, &ws);

	if (!res) {
		*width = ws.ws_col;
		*height = ws.ws_row;
		return TRUE;
	}
	else
		return FALSE;
#else
	return FALSE;
#endif
}

#endif /* !PLATFORM_WIN32 */
