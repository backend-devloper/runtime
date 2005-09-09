/*
 * exception.c: Exception handling
 *
 * Authors:
 *	Paolo Molaro    (lupus@ximian.com)
 *	Dietmar Maurer  (dietmar@ximian.com)
 *	Dick Porter     (dick@ximian.com)
 *      Miguel de Icaza (miguel@ximian.com)
 *
 * (C) 2001, 2002 Ximian, Inc.
 */

#include <mono/metadata/exception.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/appdomain.h>
#include <string.h>

/**
 * mono_exception_from_name:
 * @image: the Mono image where to look for the class
 * @name_space: the namespace for the class
 * @name: class name
 *
 * Creates an exception of the given namespace/name class.
 *
 * Returns: the initialized exception instance.
 */
MonoException *
mono_exception_from_name (MonoImage *image, const char *name_space,
			  const char *name)
{
	return mono_exception_from_name_domain (mono_domain_get (), image, name_space, name);
}

MonoException *
mono_exception_from_name_domain (MonoDomain *domain, MonoImage *image, 
				 const char* name_space, const char *name)
{
	MonoClass *klass;
	MonoObject *o;

	klass = mono_class_from_name (image, name_space, name);

	o = mono_object_new (domain, klass);
	g_assert (o != NULL);
	
	mono_runtime_object_init (o);

	return (MonoException *)o;
}


/**
 * mono_exception_from_token:
 * @image: the Mono image where to look for the class
 * @token: The type token of the class
 *
 * Creates an exception of the type given by @token.
 *
 * Returns: the initialized exception instance.
 */
MonoException *
mono_exception_from_token (MonoImage *image, guint32 token)
{
	MonoClass *klass;
	MonoObject *o;

	klass = mono_class_get (image, token);

	o = mono_object_new (mono_domain_get (), klass);
	g_assert (o != NULL);
	
	mono_runtime_object_init (o);

	return (MonoException *)o;
}

/**
 * mono_exception_from_name_two_strings:
 * @image: the Mono image where to look for the class
 * @name_space: the namespace for the class
 * @name: class name
 * @a1: first string argument to pass
 * @a2: second string argument to pass
 *
 * Creates an exception from a constructor that takes two string
 * arguments.
 *
 * Returns: the initialized exception instance.
 */
MonoException *
mono_exception_from_name_two_strings (MonoImage *image, const char *name_space,
				      const char *name, MonoString *a1, MonoString *a2)
{
	MonoDomain *domain = mono_domain_get ();
	MonoClass *klass;
	MonoMethod *method = NULL;
	MonoObject *o;
	int count = 1;
	gpointer args [2];
	gpointer iter;
	MonoMethod *m;

	if (a2 != NULL)
		count++;
	
	klass = mono_class_from_name (image, name_space, name);
	o = mono_object_new (domain, klass);

	iter = NULL;
	while ((m = mono_class_get_methods (klass, &iter))) {
		MonoMethodSignature *sig;
		
		if (strcmp (".ctor", mono_method_get_name (m)))
			continue;
		sig = mono_method_signature (m);
		if (sig->param_count != count)
			continue;

		if (sig->params [0]->type != MONO_TYPE_STRING)
			continue;
		if (count == 2 && sig->params [1]->type != MONO_TYPE_STRING)
			continue;
		method = m;
	}

	args [0] = a1;
	args [1] = a2;
	mono_runtime_invoke (method, o, args, NULL);
	return (MonoException *) o;
}

/**
 * mono_exception_from_name_msg:
 * @image: the Mono image where to look for the class
 * @name_space: the namespace for the class
 * @name: class name
 * @msg: the message to embed inside the exception
 *
 * Creates an exception and initializes its message field.
 *
 * Returns: the initialized exception instance.
 */
MonoException *
mono_exception_from_name_msg (MonoImage *image, const char *name_space,
			      const char *name, const char *msg)
{
	MonoException *ex;

	ex = mono_exception_from_name (image, name_space, name);

	if (msg)
		ex->message = mono_string_new (mono_object_get_domain ((MonoObject*)ex), msg);

	return ex;
}

MonoException *
mono_get_exception_divide_by_zero ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "DivideByZeroException");
}

MonoException *
mono_get_exception_security ()
{
	return mono_exception_from_name (mono_get_corlib (), "System.Security",
					 "SecurityException");
}

MonoException *
mono_get_exception_thread_abort ()
{
	return mono_exception_from_name (mono_get_corlib (), "System.Threading",
					 "ThreadAbortException");
}

MonoException *
mono_get_exception_arithmetic ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "ArithmeticException");
}

MonoException *
mono_get_exception_overflow ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "OverflowException");
}

MonoException *
mono_get_exception_null_reference ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "NullReferenceException");
}

MonoException *
mono_get_exception_execution_engine (const char *msg)
{
	return mono_exception_from_name_msg (mono_get_corlib (), "System",
										 "ExecutionEngineException", msg);
}

MonoException *
mono_get_exception_serialization (const char *msg)
{
	return mono_exception_from_name_msg (mono_get_corlib (), "System.Runtime.Serialization",
										 "SerializationException", msg);
}

MonoException *
mono_get_exception_invalid_cast ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "InvalidCastException");
}

MonoException *
mono_get_exception_invalid_operation (const char *msg)
{
	return mono_exception_from_name_msg (mono_get_corlib (), "System",
					"InvalidOperationException", msg);
}

MonoException *
mono_get_exception_index_out_of_range ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "IndexOutOfRangeException");
}

MonoException *
mono_get_exception_array_type_mismatch ()
{
	return mono_exception_from_name (mono_get_corlib (), "System",
					 "ArrayTypeMismatchException");
}

MonoException *
mono_get_exception_type_load (MonoString *class_name, char *assembly_name)
{
	MonoString *s = assembly_name ? mono_string_new (mono_domain_get (), assembly_name) : mono_string_new (mono_domain_get (), "");

	return mono_exception_from_name_two_strings (mono_get_corlib (), "System",
												 "TypeLoadException", class_name, s);
}

MonoException *
mono_get_exception_not_implemented (const char *msg)
{
	MonoException *ex;
	
	ex = mono_exception_from_name (mono_get_corlib (), "System",
				       "NotImplementedException");

	if (msg)
		ex->message = mono_string_new (mono_object_get_domain ((MonoObject*)ex), msg);

	return ex;
}

MonoException *
mono_get_exception_missing_method (const char *class_name, const char *member_name)
{
	MonoString *s1 = mono_string_new (mono_domain_get (), class_name);
	MonoString *s2 = mono_string_new (mono_domain_get (), member_name);

	return mono_exception_from_name_two_strings (mono_get_corlib (), "System",
												 "MissingMethodException", s1, s2);
}

MonoException *
mono_get_exception_missing_field (const char *class_name, const char *member_name)
{
	MonoString *s1 = mono_string_new (mono_domain_get (), class_name);
	MonoString *s2 = mono_string_new (mono_domain_get (), member_name);

	return mono_exception_from_name_two_strings (mono_get_corlib (), "System",
												 "MissingFieldException", s1, s2);
}

MonoException*
mono_get_exception_argument_null (const char *arg)
{
	MonoException *ex;

	ex = mono_exception_from_name ( 
		mono_get_corlib (), "System", "ArgumentNullException");

	if (arg)
		((MonoArgumentException *)ex)->param_name =
			mono_string_new (mono_object_get_domain ((MonoObject*)ex), arg);
	
	return ex;
}

MonoException *
mono_get_exception_argument (const char *arg, const char *msg)
{
	MonoException *ex;

	ex = mono_exception_from_name_msg (
		mono_get_corlib (), "System", "ArgumentException", msg);

	if (arg)
		((MonoArgumentException *)ex)->param_name =
			mono_string_new (mono_object_get_domain ((MonoObject*)ex), arg);
	
	return ex;
}

MonoException *
mono_get_exception_argument_out_of_range (const char *arg)
{
	MonoException *ex;

	ex = mono_exception_from_name (
		mono_get_corlib (), "System", "ArgumentOutOfRangeException");

	if (arg)
		((MonoArgumentException *)ex)->param_name =
			mono_string_new (mono_object_get_domain ((MonoObject*)ex), arg);
	
	return ex;
}

MonoException *
mono_get_exception_thread_state (const char *msg)
{
	return mono_exception_from_name_msg (
		mono_get_corlib (), "System.Threading", "ThreadStateException", msg);
}

MonoException *
mono_get_exception_io (const char *msg)
{
	return mono_exception_from_name_msg ( 
		mono_get_corlib (), "System.IO", "IOException", msg);
}

MonoException *
mono_get_exception_file_not_found (MonoString *fname)
{
	return mono_exception_from_name_two_strings (
		mono_get_corlib (), "System.IO", "FileNotFoundException", fname, fname);
}

MonoException *
mono_get_exception_file_not_found2 (const char *msg, MonoString *fname)
{
	MonoString *s = mono_string_new (mono_domain_get (), msg);

	return mono_exception_from_name_two_strings (
		mono_get_corlib (), "System.IO", "FileNotFoundException", s, fname);
}

MonoException *
mono_get_exception_type_initialization (const gchar *type_name, MonoException *inner)
{
	MonoClass *klass;
	gpointer args [2];
	MonoObject *exc;
	MonoMethod *method;
	gpointer iter;

	klass = mono_class_from_name (mono_get_corlib (), "System", "TypeInitializationException");
	g_assert (klass);

	mono_class_init (klass);

	/* TypeInitializationException only has 1 ctor with 2 args */
	iter = NULL;
	while ((method = mono_class_get_methods (klass, &iter))) {
		if (!strcmp (".ctor", mono_method_get_name (method)) && mono_method_signature (method)->param_count == 2)
			break;
		method = NULL;
	}

	g_assert (method);

	args [0] = mono_string_new (mono_domain_get (), type_name);
	args [1] = inner;

	exc = mono_object_new (mono_domain_get (), klass);
	mono_runtime_invoke (method, exc, args, NULL);

	return (MonoException *) exc;
}

MonoException *
mono_get_exception_synchronization_lock (const char *msg)
{
	return mono_exception_from_name_msg (mono_get_corlib (), "System.Threading", "SynchronizationLockException", msg);
}

MonoException *
mono_get_exception_cannot_unload_appdomain (const char *msg)
{
	return mono_exception_from_name_msg (mono_get_corlib (), "System", "CannotUnloadAppDomainException", msg);
}

MonoException *
mono_get_exception_appdomain_unloaded (void)
{
	return mono_exception_from_name (mono_get_corlib (), "System", "AppDomainUnloadedException");
}

MonoException *
mono_get_exception_bad_image_format (const char *msg)
{
	return mono_exception_from_name_msg (mono_get_corlib (), "System", "BadImageFormatException", msg);
}	

MonoException *
mono_get_exception_stack_overflow (void)
{
	return mono_exception_from_name (mono_get_corlib (), "System", "StackOverflowException");	
}

MonoException *
mono_get_exception_reflection_type_load (MonoArray *types, MonoArray *exceptions)
{
	MonoClass *klass;
	gpointer args [2];
	MonoObject *exc;
	MonoMethod *method;

	klass = mono_class_from_name (mono_get_corlib (), "System.Reflection", "ReflectionTypeLoadException");
	g_assert (klass);
	mono_class_init (klass);

	method = mono_class_get_method_from_name (klass, ".ctor", 2);
	g_assert (method);

	args [0] = types;
	args [1] = exceptions;

	exc = mono_object_new (mono_domain_get (), klass);
	mono_runtime_invoke (method, exc, args, NULL);

	return (MonoException *) exc;
}
