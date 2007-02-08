//
// cominterop.cs:
//
//  Tests for COM Interop related features
//

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class Tests
{

	[DllImport("libtest")]
	public static extern int mono_test_marshal_bstr_in([MarshalAs(UnmanagedType.BStr)]string str);

	[DllImport("libtest")]
    public static extern int mono_test_marshal_bstr_out([MarshalAs(UnmanagedType.BStr)] out string str);

    [DllImport("libtest")]
    public static extern int mono_test_marshal_bstr_in_null([MarshalAs(UnmanagedType.BStr)]string str);

    [DllImport("libtest")]
    public static extern int mono_test_marshal_bstr_out_null([MarshalAs(UnmanagedType.BStr)] out string str);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_sbyte([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_byte([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_short([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_ushort([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_int([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_uint([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_long([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_ulong([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_float([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_double([MarshalAs(UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_in_bstr ([MarshalAs (UnmanagedType.Struct)]object obj);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_bool_true ([MarshalAs (UnmanagedType.Struct)]object obj);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_bool_false ([MarshalAs (UnmanagedType.Struct)]object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_sbyte([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_byte([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_short([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_ushort([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_int([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_uint([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_long([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_ulong([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_float([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_double([MarshalAs(UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_bstr ([MarshalAs (UnmanagedType.Struct)]out object obj);

	[DllImport("libtest")]
	public static extern int mono_test_marshal_variant_out_bool_true ([MarshalAs (UnmanagedType.Struct)]out object obj);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_bool_false ([MarshalAs (UnmanagedType.Struct)]out object obj);


	public delegate int VarFunc (VarEnum vt, [MarshalAs (UnmanagedType.Struct)] object obj);

	public delegate int VarRefFunc (VarEnum vt, [MarshalAs (UnmanagedType.Struct)] ref object obj);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_sbyte_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_byte_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_short_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_ushort_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_int_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_uint_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_long_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_ulong_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_float_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_double_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_bstr_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_bool_true_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_in_bool_false_unmanaged (VarFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_sbyte_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_byte_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_short_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_ushort_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_int_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_uint_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_long_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_ulong_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_float_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_double_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_bstr_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_bool_true_unmanaged (VarRefFunc func);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_variant_out_bool_false_unmanaged (VarRefFunc func);

    [DllImport ("libtest")]
	public static extern int mono_test_marshal_com_object_create (out IntPtr pUnk);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_com_object_same (out IntPtr pUnk);

    [DllImport ("libtest")]
    public static extern int mono_test_marshal_com_object_destroy (IntPtr pUnk);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_com_object_ref_count (IntPtr pUnk);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_ccw_identity ([MarshalAs (UnmanagedType.Interface)]ITest itest);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_ccw_reflexive ([MarshalAs (UnmanagedType.Interface)]ITest itest);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_ccw_transitive ([MarshalAs (UnmanagedType.Interface)]ITest itest);

	[DllImport ("libtest")]
	public static extern int mono_test_marshal_ccw_itest ([MarshalAs (UnmanagedType.Interface)]ITest itest);

	public static int Main() {

        bool isWindows = !(((int)Environment.OSVersion.Platform == 4) || 
            ((int)Environment.OSVersion.Platform == 128));
		if (isWindows) {
			#region BSTR Tests

			string str;
			if (mono_test_marshal_bstr_in ("mono_test_marshal_bstr_in") != 0)
				return 1;
			if (mono_test_marshal_bstr_out (out str) != 0 || str != "mono_test_marshal_bstr_out")
                return 2;
            if (mono_test_marshal_bstr_in_null (null) != 0)
                return 1;
            if (mono_test_marshal_bstr_out_null (out str) != 0 || str != null)
                return 2;

			#endregion // BSTR Tests

			#region VARIANT Tests

			object obj;
			if (mono_test_marshal_variant_in_sbyte ((sbyte)100) != 0)
				return 13;
			if (mono_test_marshal_variant_in_byte ((byte)100) != 0)
				return 14;
			if (mono_test_marshal_variant_in_short ((short)314) != 0)
				return 15;
			if (mono_test_marshal_variant_in_ushort ((ushort)314) != 0)
				return 16;
			if (mono_test_marshal_variant_in_int ((int)314) != 0)
				return 17;
			if (mono_test_marshal_variant_in_uint ((uint)314) != 0)
				return 18;
			if (mono_test_marshal_variant_in_long ((long)314) != 0)
				return 19;
			if (mono_test_marshal_variant_in_ulong ((ulong)314) != 0)
				return 20;
			if (mono_test_marshal_variant_in_float ((float)3.14) != 0)
				return 21;
			if (mono_test_marshal_variant_in_double ((double)3.14) != 0)
				return 22;
			if (mono_test_marshal_variant_in_bstr ("PI") != 0)
				return 23;
			if (mono_test_marshal_variant_out_sbyte (out obj) != 0 || (sbyte)obj != 100)
				return 24;
			if (mono_test_marshal_variant_out_byte (out obj) != 0 || (byte)obj != 100)
				return 25;
			if (mono_test_marshal_variant_out_short (out obj) != 0 || (short)obj != 314)
				return 26;
			if (mono_test_marshal_variant_out_ushort (out obj) != 0 || (ushort)obj != 314)
				return 27;
			if (mono_test_marshal_variant_out_int (out obj) != 0 || (int)obj != 314)
				return 28;
			if (mono_test_marshal_variant_out_uint (out obj) != 0 || (uint)obj != 314)
				return 29;
			if (mono_test_marshal_variant_out_long (out obj) != 0 || (long)obj != 314)
				return 30;
			if (mono_test_marshal_variant_out_ulong (out obj) != 0 || (ulong)obj != 314)
				return 31;
			if (mono_test_marshal_variant_out_float (out obj) != 0 || ((float)obj - 3.14) / 3.14 > .001)
				return 32;
			if (mono_test_marshal_variant_out_double (out obj) != 0 || ((double)obj - 3.14) / 3.14 > .001)
				return 33;
			if (mono_test_marshal_variant_out_bstr (out obj) != 0 || (string)obj != "PI")
				return 34;

			VarFunc func = new VarFunc (mono_test_marshal_variant_in_callback);
			if (mono_test_marshal_variant_in_sbyte_unmanaged (func) != 0)
				return 35;
			if (mono_test_marshal_variant_in_byte_unmanaged (func) != 0)
				return 36;
			if (mono_test_marshal_variant_in_short_unmanaged (func) != 0)
				return 37;
			if (mono_test_marshal_variant_in_ushort_unmanaged (func) != 0)
				return 38;
			if (mono_test_marshal_variant_in_int_unmanaged (func) != 0)
				return 39;
			if (mono_test_marshal_variant_in_uint_unmanaged (func) != 0)
				return 40;
			if (mono_test_marshal_variant_in_long_unmanaged (func) != 0)
				return 41;
			if (mono_test_marshal_variant_in_ulong_unmanaged (func) != 0)
				return 42;
			if (mono_test_marshal_variant_in_float_unmanaged (func) != 0)
				return 43;
			if (mono_test_marshal_variant_in_double_unmanaged (func) != 0)
				return 44;
			if (mono_test_marshal_variant_in_bstr_unmanaged (func) != 0)
				return 45;
			if (mono_test_marshal_variant_in_bool_true_unmanaged (func) != 0)
				return 46;

			VarRefFunc reffunc = new VarRefFunc (mono_test_marshal_variant_out_callback);
			if (mono_test_marshal_variant_out_sbyte_unmanaged (reffunc) != 0)
				return 50;
			if (mono_test_marshal_variant_out_byte_unmanaged (reffunc) != 0)
				return 51;
			if (mono_test_marshal_variant_out_short_unmanaged (reffunc) != 0)
				return 52;
			if (mono_test_marshal_variant_out_ushort_unmanaged (reffunc) != 0)
				return 53;
			if (mono_test_marshal_variant_out_int_unmanaged (reffunc) != 0)
				return 54;
			if (mono_test_marshal_variant_out_uint_unmanaged (reffunc) != 0)
				return 55;
			if (mono_test_marshal_variant_out_long_unmanaged (reffunc) != 0)
				return 56;
			if (mono_test_marshal_variant_out_ulong_unmanaged (reffunc) != 0)
				return 57;
			if (mono_test_marshal_variant_out_float_unmanaged (reffunc) != 0)
				return 58;
			if (mono_test_marshal_variant_out_double_unmanaged (reffunc) != 0)
				return 59;
			if (mono_test_marshal_variant_out_bstr_unmanaged (reffunc) != 0)
				return 60;
			if (mono_test_marshal_variant_out_bool_true_unmanaged (reffunc) != 0)
				return 61;

			#endregion // VARIANT Tests

			#region Runtime Callable Wrapper Tests

			IntPtr pUnk;
			if (mono_test_marshal_com_object_create (out pUnk) != 0)
				return 145;

			if (mono_test_marshal_com_object_ref_count (pUnk) != 1)
				return 146;

			if (Marshal.AddRef (pUnk) != 2)
				return 147;

			if (mono_test_marshal_com_object_ref_count (pUnk) != 2)
				return 148;

			if (Marshal.Release (pUnk) != 1)
				return 149;

			if (mono_test_marshal_com_object_ref_count (pUnk) != 1)
				return 150;

			object com_obj = Marshal.GetObjectForIUnknown (pUnk);

			if (com_obj == null)
				return 151;

			ITest itest = com_obj as ITest;

			if (itest == null)
				return 152;

			IntPtr pUnk2;
			if (mono_test_marshal_com_object_same (out pUnk2) != 0)
				return 153;

			object com_obj2 = Marshal.GetObjectForIUnknown (pUnk2);
			
			if (com_obj != com_obj2)
				return 154;

			if (!com_obj.Equals (com_obj2))
				return 155;

			IntPtr pUnk3;
			if (mono_test_marshal_com_object_create (out pUnk3) != 0)
				return 156;

			object com_obj3 = Marshal.GetObjectForIUnknown (pUnk3);
			if (com_obj == com_obj3)
				return 157;

			if (com_obj.Equals (com_obj3))
				return 158;

			// com_obj & com_obj2 share a RCW
			if (Marshal.ReleaseComObject (com_obj2) != 1)
				return 159;

			// com_obj3 should only have one RCW
			if (Marshal.ReleaseComObject (com_obj3) != 0)
				return 160;

			IntPtr iunknown = Marshal.GetIUnknownForObject (com_obj);
			if (iunknown == IntPtr.Zero)
				return 170;

			if (pUnk != iunknown)
				return 171;

			#endregion // Runtime Callable Wrapper Tests

			#region COM Callable Wrapper Tests

			ManagedTest test = new ManagedTest ();

			mono_test_marshal_ccw_itest (test);

			if (test.Status != 0)
				return 200;

			#endregion // COM Callable Wrapper Tests
		}

        return 0;
	}


	[ComImport ()]
	[Guid ("00000000-0000-0000-0000-000000000001")]
	[InterfaceType (ComInterfaceType.InterfaceIsIUnknown)]
	public interface ITest
	{
		// properties need to go first since mcs puts them there
		ITest Test
		{
			[return: MarshalAs (UnmanagedType.Interface)]
			[MethodImpl (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime), DispId (5242884)]
			get;
		}

		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void SByteIn (sbyte val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void ByteIn (byte val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void ShortIn (short val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void UShortIn (ushort val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void IntIn (int val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void UIntIn (uint val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void LongIn (long val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void ULongIn (ulong val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void FloatIn (float val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void DoubleIn (double val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void ITestIn ([MarshalAs (UnmanagedType.Interface)]ITest val);
		[MethodImplAttribute (MethodImplOptions.InternalCall, MethodCodeType = MethodCodeType.Runtime)]
		void ITestOut ([MarshalAs (UnmanagedType.Interface)]out ITest val);
	}

	public class ManagedTest : ITest
	{
		private int status = 0;
		public int Status
		{
			get { return status; }
		}
		public void SByteIn (sbyte val)
		{
			if (val != -100)
				status = 1;
		}

		public void ByteIn (byte val)
		{
			if (val != 100)
				status = 2;
		}

		public void ShortIn (short val)
		{
			if (val != -100)
				status = 3;
		}

		public void UShortIn (ushort val)
		{
			if (val != 100)
				status = 4;
		}

		public void IntIn (int val)
		{
			if (val != -100)
				status = 5;
		}

		public void UIntIn (uint val)
		{
			if (val != 100)
				status = 6;
		}

		public void LongIn (long val)
		{
			if (val != -100)
				status = 7;
		}

		public void ULongIn (ulong val)
		{
			if (val != 100)
				status = 8;
		}

		public void FloatIn (float val)
		{
			if (Math.Abs (val - 3.14f) > .000001)
				status = 9;
		}

		public void DoubleIn (double val)
		{
			if (Math.Abs (val - 3.14) > .000001)
				status = 10;
		}

		public void ITestIn (ITest val)
		{
			if (val == null)
				status = 11;
			if (null == val as ManagedTest)
				status = 12;
		}

		public void ITestOut (out ITest val)
		{
			val = new ManagedTest ();
		}

		public ITest Test
		{
			get
			{
				return new ManagedTest ();
			}
		}
	}

	public static int mono_test_marshal_variant_in_callback (VarEnum vt, object obj)
	{
		switch (vt)
		{
		case VarEnum.VT_I1:
			if (obj.GetType () != typeof (sbyte))
				return 1;
			if ((sbyte)obj != -100)
				return 2;
			break;
		case VarEnum.VT_UI1:
			if (obj.GetType () != typeof (byte))
				return 1;
			if ((byte)obj != 100)
				return 2;
			break;
		case VarEnum.VT_I2:
			if (obj.GetType () != typeof (short))
				return 1;
			if ((short)obj != -100)
				return 2;
			break;
		case VarEnum.VT_UI2:
			if (obj.GetType () != typeof (ushort))
				return 1;
			if ((ushort)obj != 100)
				return 2;
			break;
		case VarEnum.VT_I4:
			if (obj.GetType () != typeof (int))
				return 1;
			if ((int)obj != -100)
				return 2;
			break;
		case VarEnum.VT_UI4:
			if (obj.GetType () != typeof (uint))
				return 1;
			if ((uint)obj != 100)
				return 2;
			break;
		case VarEnum.VT_I8:
			if (obj.GetType () != typeof (long))
				return 1;
			if ((long)obj != -100)
				return 2;
			break;
		case VarEnum.VT_UI8:
			if (obj.GetType () != typeof (ulong))
				return 1;
			if ((ulong)obj != 100)
				return 2;
			break;
		case VarEnum.VT_R4:
			if (obj.GetType () != typeof (float))
				return 1;
			if (Math.Abs ((float)obj - 3.14f) > 1e-10)
				return 2;
			break;
		case VarEnum.VT_R8:
			if (obj.GetType () != typeof (double))
				return 1;
			if (Math.Abs ((double)obj - 3.14) > 1e-10)
				return 2;
			break;
		case VarEnum.VT_BSTR:
			if (obj.GetType () != typeof (string))
				return 1;
			if ((string)obj != "PI")
				return 2;
			break;
		case VarEnum.VT_BOOL:
			if (obj.GetType () != typeof (bool))
				return 1;
			if ((bool)obj != true)
				return 2;
			break;
		}
		return 0;
	}

	public static int mono_test_marshal_variant_out_callback (VarEnum vt, ref object obj)
	{
		switch (vt) {
		case VarEnum.VT_I1:
			obj = (sbyte)-100;
			break;
		case VarEnum.VT_UI1:
			obj = (byte)100;
			break;
		case VarEnum.VT_I2:
			obj = (short)-100;
			break;
		case VarEnum.VT_UI2:
			obj = (ushort)100;
			break;
		case VarEnum.VT_I4:
			obj = (int)-100;
			break;
		case VarEnum.VT_UI4:
			obj = (uint)100;
			break;
		case VarEnum.VT_I8:
			obj = (long)-100;
			break;
		case VarEnum.VT_UI8:
			obj = (ulong)100;
			break;
		case VarEnum.VT_R4:
			obj = (float)3.14f;
			break;
		case VarEnum.VT_R8:
			obj = (double)3.14;
			break;
		case VarEnum.VT_BSTR:
			obj = "PI";
			break;
		case VarEnum.VT_BOOL:
			obj = true;
			break;
		}
		return 0;
	}
}
