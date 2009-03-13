using System.Security;
using System;
using System.Runtime.InteropServices;
using System.Reflection;

[SecurityCriticalAttribute]
public class CClass
{
	public CClass ()
	{
		//Console.WriteLine ("c ctor");
	}

	public virtual void Method ()
	{
		//Console.WriteLine ("c class");
	}

	public static void StaticMethod ()
	{
		//Console.WriteLine ("c class static");
	}
}

[SecuritySafeCriticalAttribute]
public class SCClass
{
	public SCClass ()
	{
		//Console.WriteLine ("sc ctor");
	}

	public virtual void Method ()
	{
		//Console.WriteLine ("sc class");
		CClass cc = new CClass ();
		cc.Method ();
	}
}

public class SCDevClass : SCClass
{
	public SCDevClass ()
	{
		Test.error ("safe-critical-derived class instantiated");
	}

	public override void Method ()
	{
		//base.Method ();
		Test.error ("safe-critical-derived method called");
	}
}

public class CMethodClass
{
	public CMethodClass ()
	{
		//Console.WriteLine ("cmethod ctor");
	}

	[SecurityCriticalAttribute]
	public virtual void Method ()
	{
		//Console.WriteLine ("cmethod");
	}
}

public class CMethodDevClass : CMethodClass
{
	public CMethodDevClass ()
	{
		Test.error ("critical-derived constructor called");
	}

	public override void Method ()
	{
		//base.Method();
		Test.error ("critical-derived method called");
	}
}

public interface CMethodInterface {
	[SecurityCriticalAttribute]
	void Method ();
}

public class CInterfaceClass : CMethodInterface {
	public CInterfaceClass () { }

	public void Method ()
	{
		Test.error ("security-critical-interface-derived method called");
	}
}

[SecurityCriticalAttribute]
public class CriticalClass {

        public class NestedClassInsideCritical {

                static public void Method ()
                {
                        Test.error ("critical inner class method called");
                }
        }
}

public delegate void MethodDelegate ();

public delegate Object InvokeDelegate (Object obj, Object[] parms);

public class Test
{
	static bool haveError = false;

	public static void error (string text)
	{
		Console.WriteLine (text);
		haveError = true;
	}

	[SecurityCriticalAttribute]
	static void CMethod ()
	{
		//Console.WriteLine ("c");
	}

	[SecuritySafeCriticalAttribute]
	static void SCMethod ()
	{
		//Console.WriteLine ("sc");
		CMethod ();
	}

	static void doSCDev ()
	{
		SCDevClass scdev = new SCDevClass ();
		scdev.Method ();
	}

	static void doCMethodDev ()
	{
		CMethodDevClass cmdev = new CMethodDevClass ();
		error ("critical-derived object instantiated");
		cmdev.Method ();
		Console.WriteLine ("critical-derived method called");
	}

	static void doSCInterfaceDev ()
	{
		CMethodInterface mi = new CInterfaceClass ();
		error ("safe-critical-interface-derived object instantiated");
		mi.Method ();
		error ("safe-critical-interface-derived method called");
	}

	/*
	static unsafe void unsafeMethod ()
	{
		byte *p = null;
		error ("unsafe method called");
	}
	*/

	public static void TransparentReflectionCMethod ()
	{
	}

	[SecurityCriticalAttribute]
	public static void ReflectionCMethod ()
	{
		error ("method called via reflection");
	}

	[SecurityCriticalAttribute]
	public static unsafe void StringTest ()
	{
		string str = "blabla";
		char [] arr = str.ToCharArray ();
		string r;

		fixed (char *tarr = arr) {
			int ss = 1, l = 3;
			r = new string (tarr, ss, l - ss);
		}
	}

	[SecuritySafeCriticalAttribute]
	public static void CallStringTest ()
	{
		StringTest ();
	}

	[DllImport ("/lib64/libc.so.6")]
	static extern int getpid ();

	public static int Main ()
	{
		SCMethod ();

		try {
			CMethod ();
			error ("static critical method called");
		} catch (MethodAccessException) {
		}

		SCClass sc = new SCClass ();
		sc.Method ();

		try {
			CClass c = new CClass (); // Illegal
			error ("critical object instantiated");
			c.Method ();	// Illegal
			error ("critical method called");
		} catch (MethodAccessException) {
		}

		try {
			doSCDev ();
			error ("security-critical-derived class error");
		} catch (TypeLoadException) {
		}

		try {
			doCMethodDev ();
		} catch (TypeLoadException) {
		}

		try {
			getpid ();
			error ("pinvoke called");
		} catch (MethodAccessException) {
		}

		try {
			MethodDelegate md = new MethodDelegate (CClass.StaticMethod);
			md ();
			error ("critical method called via delegate");
		} catch (MethodAccessException) {
		}

		try {
			CriticalClass.NestedClassInsideCritical.Method ();
		} catch (MethodAccessException) {
		}

		try {
			doSCInterfaceDev ();
		} catch (TypeLoadException) {
		}

		/*
		try {
			unsafeMethod ();
		} catch (VerificationException) {
		}
		*/

		try {
			Type type = Type.GetType ("Test");
			MethodInfo method = type.GetMethod ("TransparentReflectionCMethod");

			method.Invoke(null, null);
		} catch (MethodAccessException) {
			error ("transparent method not called via reflection");
		}

		try {
			Type type = Type.GetType ("Test");
			MethodInfo method = type.GetMethod ("ReflectionCMethod");

			method.Invoke(null, null);
		} catch (MethodAccessException) {
		}

		try {
			Type type = Type.GetType ("Test");
			MethodInfo method = type.GetMethod ("TransparentReflectionCMethod");
			InvokeDelegate id = new InvokeDelegate (method.Invoke);

			id (null, null);
		} catch (MethodAccessException) {
			error ("transparent method not called via reflection delegate");
		}

		try {
			Type type = Type.GetType ("Test");
			MethodInfo method = type.GetMethod ("ReflectionCMethod");
			InvokeDelegate id = new InvokeDelegate (method.Invoke);

			id (null, null);
		} catch (MethodAccessException) {
		}


		// wrapper 7
		try {
			CallStringTest ();
		} catch (MethodAccessException) {
			error ("string test failed");
		}

		//Console.WriteLine ("ok");

		if (haveError)
			return 1;

		return 0;
	}
}
