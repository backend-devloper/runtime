using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

/*
 * Regression tests for the mono JIT.
 *
 * Each test needs to be of the form:
 *
 * static int test_<result>_<name> ();
 *
 * where <result> is an integer (the value that needs to be returned by
 * the method to make it pass.
 * <name> is a user-displayed name used to identify the test.
 *
 * The tests can be driven in two ways:
 * *) running the program directly: Main() uses reflection to find and invoke
 * 	the test methods (this is useful mostly to check that the tests are correct)
 * *) with the --regression switch of the jit (this is the preferred way since
 * 	all the tests will be run with optimizations on and off)
 *
 * The reflection logic could be moved to a .dll since we need at least another
 * regression test file written in IL code to have better control on how
 * the IL code looks.
 */

struct Simple {
	public int a;
	public byte b;
	public short c;
	public long d;
}

struct Small {
	public byte b1;
	public byte b2;
}

// Size=2, Align=1
struct Foo {
	bool b1;
	bool b2;
}

struct Large {
	int one;
	int two;
	long three;
	long four;
	int five;
	long six;
	int seven;
	long eight;
	long nine;
	long ten;

	public void populate ()
	{
		one = 1; two = 2;
		three = 3; four = 4;
		five = 5; six = 6;
		seven = 7; eight = 8;
		nine = 9; ten = 10;
	}
	public bool check ()
	{
		return one == 1  && two == 2  &&
			three == 3  && four == 4  &&
			five == 5  && six == 6  &&
			seven == 7  && eight == 8  &&
			nine == 9  && ten == 10;
	}
}

class Sample {
	public int a;
	public Sample (int v) {
		a = v;
	}
}

[StructLayout ( LayoutKind.Explicit )]
struct StructWithBigOffsets {
		[ FieldOffset(10000) ] public byte b;
		[ FieldOffset(10001) ] public sbyte sb;
		[ FieldOffset(11000) ] public short s;
		[ FieldOffset(11002) ] public ushort us;
		[ FieldOffset(12000) ] public uint i;
		[ FieldOffset(12004) ] public int si;
		[ FieldOffset(13000) ] public long l;
		[ FieldOffset(14000) ] public float f;
		[ FieldOffset(15000) ] public double d;
}

enum SampleEnum {
	A,
	B,
	C
}

class Tests {

	static int Main () {
		return TestDriver.RunTests (typeof (Tests));
	}
	
	static int test_0_return () {
		Simple s;
		s.a = 1;
		s.b = 2;
		s.c = (short)(s.a + s.b);
		s.d = 4;
		return s.a - 1;
	}

	static int test_0_string_access () {
		string s = "Hello";
		if (s [1] != 'e')
			return 1;
		return 0;
	}

	static int test_0_string_virtual_call () {
		string s = "Hello";
		string s2 = s.ToString ();
		if (s2 [1] != 'e')
			return 1;
		return 0;
	}

	static int test_0_iface_call () {
		string s = "Hello";
		object o = ((ICloneable)s).Clone ();
		return 0;
	}

	static int test_5_newobj () {
		Sample s = new Sample (5);
		return s.a;
	}

	static int test_4_box () {
		object obj = 4;
		return (int)obj;
	}

	static int test_0_enum_unbox () {
		SampleEnum x = SampleEnum.A;
		object o = x;
		
		int res = 1;

		res = (int)o;
		
		return res;
	}
	
	static Simple get_simple (int v) {
		Simple r = new Simple ();
		r.a = v;
		r.b = (byte)(v + 1);
		r.c = (short)(v + 2);
		r.d = v + 3;

		return r;
	}

	static int test_3_return_struct () {
		Simple v = get_simple (1);

		if (v.a != 1)
			return 0;
		if (v.b != 2)
			return 0;
		if (v.c != 3)
			return 0;
		if (v.d != 4)
			return 0;
		return 3;
	}

	public virtual Simple v_get_simple (int v)
	{
		return get_simple (v);
	}
	
	static int test_2_return_struct_virtual () {
		Tests t = new Tests ();
		Simple v = t.v_get_simple (2);

		if (v.a != 2)
			return 0;
		if (v.b != 3)
			return 0;
		if (v.c != 4)
			return 0;
		if (v.d != 5)
			return 0;
		return 2;
	}

	static int receive_simple (int a, Simple v, int b) {
		if (v.a != 1)
			return 1;
		if (v.b != 2)
			return 2;
		if (v.c != 3)
			return 3;
		if (v.d != 4)
			return 4;
		if (a != 7)
			return 5;
		if (b != 9)
			return 6;
		return 0;
	}
	
	static int test_5_pass_struct () {
		Simple v = get_simple (1);
		if (receive_simple (7, v, 9) != 0)
			return 0;
		if (receive_simple (7, get_simple (1), 9) != 0)
			return 1;
		return 5;
	}

	// Test alignment of small structs

	static Small get_small (byte v) {
		Small r = new Small ();
	
		r.b1 = v;
		r.b2 = (byte)(v + 1);

		return r;
	}

	static Small return_small (Small s) {
		return s;
	}

	static int receive_small (int a, Small v, int b) {
		if (v.b1 != 1)
			return 1;
		if (v.b2 != 2)
			return 2;
		return 0;
	}

	static int test_5_pass_small_struct () {
		Small v = get_small (1);
		if (receive_small (7, v, 9) != 0)
			return 0;
		if (receive_small (7, get_small (1), 9) != 0)
			return 1;
		v = return_small (v);
		if (v.b1 != 1)
			return 2;
		if (v.b2 != 2)
			return 3;
		return 5;
	}

	struct AStruct {
		public int i;

		public AStruct (int i) {
			this.i = i;
		}

		public override int GetHashCode () {
			return i;
		}
	}

	// Test that vtypes are unboxed during a virtual call
	static int test_44_unbox_trampoline () {
		AStruct s = new AStruct (44);
		object o = s;
		return o.GetHashCode ();
	}

	static int test_0_unbox_trampoline2 () {
		int i = 12;
		object o = i;
			
		if (i.ToString () != "12")
			return 1;
		if (((Int32)o).ToString () != "12")
			return 2;
		if (o.ToString () != "12")
			return 3;
		return 0;
	}

	// Test fields with big offsets
	static int test_0_fields_with_big_offsets () {
		StructWithBigOffsets s = new StructWithBigOffsets ();
		StructWithBigOffsets s2 = new StructWithBigOffsets ();

		s.b = 0xde;
		s.sb = 0xe;
		s.s = 0x12de;
		s.us = 0x12da;
		s.i = 0xdeadbeef;
		s.si = 0xcafe;
		s.l = 0xcafebabe;
		s.f = 3.14F;
		s.d = 3.14;

		s2.b = s.b;
		s2.sb = s.sb;
		s2.s = s.s;
		s2.us = s.us;
		s2.i = s.i;
		s2.si = s.si;
		s2.l = s.l;
		s2.f = s.f;
		s2.d = s.d;

		if (s2.b != 0xde)
			return 1;
		if (s2.s != 0x12de)
			return 2;
		if (s2.i != 0xdeadbeef)
			return 3;
		if (s2.l != 0xcafebabe)
			return 4;
		if (s2.f != 3.14F)
			return 5;
		if (s2.d != 3.14)
			return 6;
		if (s2.sb != 0xe)
			return 7;
		if (s2.us != 0x12da)
			return 9;
		if (s2.si != 0xcafe)
			return 10;

		return 0;
	}

	class TestRegA {

		long buf_start;
		int buf_length, buf_offset;

		public TestRegA () {
			buf_start = 0;
			buf_length = 0;
			buf_offset = 0;
		}
	
		public long Seek (long position) {
			long pos = position;
			/* interaction between the register allocator and
			 * allocating arguments to registers */
			if (pos >= buf_start && pos <= buf_start + buf_length) {
				buf_offset = (int) (pos - buf_start);
				return pos;
			}
			return buf_start;
		}

	}

	static int test_0_seektest () {
		TestRegA t = new TestRegA ();
		return (int)t.Seek (0);
	}

	class Super : ICloneable {
		public virtual object Clone () {
			return null;
		}
	}
	class Duper: Super {
	}

	static int test_0_null_cast () {
		object o = null;

		Super s = (Super)o;

		return 0;
	}
	
	static int test_0_super_cast () {
		Duper d = new Duper ();
		Super sup = d;
		Object o = d;

		if (!(o is Super))
			return 1;
		try {
			d = (Duper)sup;
		} catch {
			return 2;
		}
		if (!(d is Object))
			return 3;
		try {
			d = (Duper)(object)sup;
		} catch {
			return 4;
		}
		return 0;
	}

	static int test_0_super_cast_array () {
		Duper[] d = new Duper [0];
		Super[] sup = d;
		Object[] o = d;

		if (!(o is Super[]))
			return 1;
		try {
			d = (Duper[])sup;
		} catch {
			return 2;
		}
		if (!(d is Object[]))
			return 3;
		try {
			d = (Duper[])(object[])sup;
		} catch {
			return 4;
		}
		return 0;
	}

	static int test_0_multi_array_cast () {
		Duper[,] d = new Duper [1, 1];
		object[,] o = d;

		try {
			o [0, 0] = new Super ();
			return 1;
		}
		catch (ArrayTypeMismatchException) {
		}

		return 0;
	}

	static int test_0_vector_array_cast () {
		Array arr1 = Array.CreateInstance (typeof (int), new int[] {1}, new int[] {0});
		Array arr2 = Array.CreateInstance (typeof (int), new int[] {1}, new int[] {10});

		if (arr1.GetType () != typeof (int[]))
			return 1;

		if (arr2.GetType () == typeof (int[]))
			return 2;

		int[] b;

		b = (int[])arr1;

		try {
			b = (int[])arr2;
			return 3;
		}
		catch (InvalidCastException) {
		}

		if (arr2 is int[])
			return 4;

		return 0;
	}

	static int test_0_enum_array_cast () {
		TypeCode[] tc = new TypeCode [0];
		object[] oa;
		ValueType[] vta;
		int[] inta;
		Array a = tc;
		bool ok;

		if (a is object[])
			return 1;
		if (a is ValueType[])
			return 2;
		if (a is Enum[])
			return 3;
		try {
			ok = false;
			oa = (object[])a;
		} catch {
			ok = true;
		}
		if (!ok)
			return 4;
		try {
			ok = false;
			vta = (ValueType[])a;
		} catch {
			ok = true;
		}
		if (!ok)
			return 5;
		try {
			ok = true;
			inta = (int[])a;
		} catch {
			ok = false;
		}
		if (!ok)
			return 6;
		return 0;
	}

	static int test_0_more_cast_corner_cases () {
		ValueType[] vta = new ValueType [0];
		Enum[] ea = new Enum [0];
		Array a = vta;
		object[] oa;
		bool ok;

		if (!(a is object[]))
			return 1;
		if (!(a is ValueType[]))
			return 2;
		if (a is Enum[])
			return 3;
		a = ea;
		if (!(a is object[]))
			return 4;
		if (!(a is ValueType[]))
			return 5;
		if (!(a is Enum[]))
			return 6;

		try {
			ok = true;
			oa = (object[])a;
		} catch {
			ok = false;
		}
		if (!ok)
			return 7;
	
		try {
			ok = true;
			oa = (Enum[])a;
		} catch {
			ok = false;
		}
		if (!ok)
			return 8;
	
		try {
			ok = true;
			oa = (ValueType[])a;
		} catch {
			ok = false;
		}
		if (!ok)
			return 9;

		a = vta;
		try {
			ok = true;
			oa = (object[])a;
		} catch {
			ok = false;
		}
		if (!ok)
			return 10;
	
		try {
			ok = true;
			oa = (ValueType[])a;
		} catch {
			ok = false;
		}
		if (!ok)
			return 11;
	
		try {
			ok = false;
			vta = (Enum[])a;
		} catch {
			ok = true;
		}
		if (!ok)
			return 12;
		return 0;
	}

	static int test_0_cast_iface_array () {
		object o = new ICloneable [0];
		object o2 = new Duper [0];
		object t;
		bool ok;

		if (!(o is object[]))
			return 1;
		if (!(o2 is ICloneable[]))
			return 2;

		try {
			ok = true;
			t = (object[])o;
		} catch {
			ok = false;
		}
		if (!ok)
			return 3;
	
		try {
			ok = true;
			t = (ICloneable[])o2;
		} catch {
			ok = false;
		}
		if (!ok)
			return 4;

		try {
			ok = true;
			t = (ICloneable[])o;
		} catch {
			ok = false;
		}
		if (!ok)
			return 5;

		if (!(o is ICloneable[]))
			return 6;

		/* add tests for interfaces that 'inherit' interfaces */
		return 0;
	}

	private static int[] daysmonthleap = { 0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	private static int AbsoluteDays (int year, int month, int day)
	{
		int temp = 0, m = 1;
		int[] days = daysmonthleap;
		while (m < month)
			temp += days[m++];
		return ((day-1) + temp + (365* (year-1)) + ((year-1)/4) - ((year-1)/100) + ((year-1)/400));
	}

	static int test_719162_complex_div () {
		int adays = AbsoluteDays (1970, 1, 1);
		return adays;
	}

	delegate int GetIntDel ();

	static int return4 () {
		return 4;
	}

	int return5 () {
		return 5;
	}

	static int test_2_static_delegate () {
		GetIntDel del = new GetIntDel (return4);
		int v = del ();
		if (v != 4)
			return 0;
		return 2;
	}

	static int test_2_instance_delegate () {
		Tests t = new Tests ();
		GetIntDel del = new GetIntDel (t.return5);
		int v = del ();
		if (v != 5)
			return 0;
		return 2;
	}

	static int test_1_store_decimal () {
		decimal[,] a = {{1}};

		if (a[0,0] != 1m)
			return 0;
		return 1;
	}

	static int test_2_intptr_stobj () {
		System.IntPtr [] arr = { new System.IntPtr () };

		if (arr [0] != (System.IntPtr)0)
			return 1;
		return 2;
	}

	static int llmult (int a, int b, int c, int d) {
		return a + b + c + d;
	}

	/* 
	 * Test that evaluation of complex arguments does not overwrite the
	 * arguments already in outgoing registers.
	 */
	static int test_155_regalloc () {
		int a = 10;
		int b = 10;

		int c = 0;
		int d = 0;
		int[] arr = new int [5];

		return llmult (arr [c + d], 150, 5, 0);
	}

	static bool large_struct_test (Large a, Large b, Large c, Large d)
	{
		if (!a.check ()) return false;
		if (!b.check ()) return false;
		if (!c.check ()) return false;
		if (!d.check ()) return false;
		return true;
	}

	static int test_2_large_struct_pass ()
	{
		Large a, b, c, d;
		a = new Large ();
		b = new Large ();
		c = new Large ();
		d = new Large ();
		a.populate ();
		b.populate ();
		c.populate ();
		d.populate ();
		if (large_struct_test (a, b, c, d))
			return 2;
		return 0;
	}

	public static unsafe int test_0_pin_string () {
		string x = "xxx";
		fixed (char *c = x) {
			if (*c != 'x')
				return 1;
		}
		return 0;
	}
	
	public static int my_flags;
	public static int test_0_and_cmp_static ()
	{
		
		/* various forms of test [mem], imm */
		
		my_flags = 0x01020304;
		
		if ((my_flags & 0x01020304) == 0)
			return 1;
		
		if ((my_flags & 0x00000304) == 0)
			return 2;
		
		if ((my_flags & 0x00000004) == 0)
			return 3;
		
		if ((my_flags & 0x00000300) == 0)
			return 4;
		
		if ((my_flags & 0x00020000) == 0)
			return 5;
		
		if ((my_flags & 0x01000000) == 0)
			return 6;
		
		return 0;
	}
	
	static byte b;
	public static int test_0_byte_compares ()
	{
		b = 0xff;
		if (b == -1)
			return 1;
		b = 0;
		if (!(b < System.Byte.MaxValue))
			return 2;
		
		if (!(b <= System.Byte.MaxValue))
			return 3;
		
		return 0;
	}

	public static int test_71_long_shift_right () {
		ulong value = 38654838087;
		int x = 0;
		byte [] buffer = new byte [1];
		buffer [x] = ((byte)(value >> x));
		return buffer [x];
	}
	
	static long x;
	public static int test_0_addsub_mem ()
	{
		x = 0;
		x += 5;
		
		if (x != 5)
			return 1;
		
		x -= 10;
		
		if (x != -5)
			return 2;
		
		return 0;
	}
	
	static ulong y;
	public static int test_0_sh32_mem ()
	{
		y = 0x0102130405060708;
		y >>= 32;
		
		if (y != 0x01021304)
			return 1;
		
		y = 0x0102130405060708;
		y <<= 32;
		
		if (y != 0x0506070800000000)
			return 2;
		
		x = 0x0102130405060708;
		x <<= 32;
		
		if (x != 0x0506070800000000)
			return 2;
		
		return 0;
	}


	static uint dum_de_dum = 1;
	static int test_0_long_arg_opt ()
	{
		return Foo (0x1234567887654321, dum_de_dum);
	}
	
	static int Foo (ulong x, ulong y)
	{
		if (x != 0x1234567887654321)
			return 1;
		
		if (y != 1)
			return 2;
		
		return 0;
	}
	
	static int test_0_long_ret_opt ()
	{
		ulong x = X ();
		if (x != 0x1234567887654321)
			return 1;
		ulong y = Y ();
		if (y != 1)
			return 2;
		
		return 0;
	}
	
	static ulong X ()
	{
		return 0x1234567887654321;
	}
	
	static ulong Y ()
	{
		return dum_de_dum;
	}

	/* from bug# 71515 */
	static int counter = 0;
	static bool WriteStuff () {
		counter = 10;
		return true;
	}
	static int test_0_cond_branch_side_effects () {
		counter = 5;
		if (WriteStuff());
		if (counter == 10)
			return 0;
		return 1;
	}

	// bug #74992
	public static int arg_only_written (string file_name, int[]
ncells ) {
		if (file_name == null)
			return 1;

		ncells = foo ();
		bar (ncells [0]);

		return 0;
	}

	public static int[] foo () {
		return new int [3];
	}

	public static void bar (int i) {
	}
	

	public static int test_0_arg_only_written ()
	{
		return arg_only_written ("md.in", null);
	}		

	static long position = 0;

	public static int test_4_static_inc_long () {

		int count = 4;

		position = 0;

		position += count;

		return (int)position;
	}

	struct FooStruct {

		public FooStruct (long l) {
		}
	}

	static int test_0_calls_opcode_emulation () {
		// Test that emulated opcodes do not clobber arguments already in
		// out registers
		checked {
			long val = 10000;
			new FooStruct (val * 10000);
		}
		return 0;
	}

	static int test_0_intrins_string_length () {
		string s = "ABC";

		return (s.Length == 3) ? 0 : 1;
	}

	static int test_0_intrins_string_chars () {
		string s = "ABC";

		return (s [0] == 'A' && s [1] == 'B' && s [2] == 'C') ? 0 : 1;
	}

	static int test_0_intrins_object_gettype () {
		object o = 1;

		return (o.GetType () == typeof (int)) ? 0 : 1;
	}

	static int test_0_intrins_object_gethashcode () {
		object o = new Object ();

		return (o.GetHashCode () == o.GetHashCode ()) ? 0 : 1;
	}

	class FooClass {
	}

	static int test_0_intrins_object_ctor () {
		object o = new FooClass ();

		return (o != null) ? 0 : 1;
	}

	static int test_0_intrins_array_rank () {
		int[,] a = new int [10, 10];

		return (a.Rank == 2) ? 0 : 1;
	}

	static int test_0_intrins_array_length () {
		int[,] a = new int [10, 10];
		Array a2 = a;

		return (a2.Length == 100) ? 0 : 1;
	}

	static int test_0_intrins_runtimehelpers_offset_to_string_data () {
		int i = RuntimeHelpers.OffsetToStringData;
		
		return i - i;
	}

	public class Bar {
		bool allowLocation = true;
        Foo f = new Foo ();	
	}

	static int test_0_regress_78990_unaligned_structs () {
		new Bar ();

		return 0;
	}
}

