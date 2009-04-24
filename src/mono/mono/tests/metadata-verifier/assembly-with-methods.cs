using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

public class TypeOne
{
	public void GenericMethod<T> () {
	}

	public void SimpleMethod () {
	}
}

public interface IFace
{
	void MyMethod ();
}


public class TypeTwo
{
	[DllImport ("bla.dll")]
	public static extern void PInvoke ();
}

public abstract class AbsClass
{
	public abstract void AbsBla ();
}

public static class InternalCall
{
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public static extern int ICall (object o);
}


public class ClassWithCCtor
{
	static ClassWithCCtor () {
	
	}
}

public class LastClass
{
	public static void Main ()
	{
	
	}
}