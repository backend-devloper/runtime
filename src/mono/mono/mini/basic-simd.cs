using System;
using Mono.Simd;

public class SimdTests {
	public static int test_0_vector4ui_sar () {
		Vector4ui a = new Vector4ui (0xF0000000u,20,3,40);
		
		Vector4ui c = Vector4ui.ShiftRightArithmetic (a, 2);
	
		if (c.X != 0xFC000000)
			return 1;
		if (c.Y != 5)
			return 2;
		if (c.Z != 0)
			return 3;
		if (c.W != 10)
			return 4;
		return 0;
	}

	public static int test_0_vector4ui_unpack_high () {
		Vector4ui a = new Vector4ui (1,2,3,4);
		Vector4ui b = new Vector4ui (5,6,7,8);
		
		Vector4ui c = Vector4ui.UnpackHigh(a, b);
	
		if (c.X != 3)
			return 1;
		if (c.Y != 7)
			return 2;
		if (c.Z != 4)
			return 3;
		if (c.W != 8)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_unpack_low () {
		Vector4ui a = new Vector4ui (1,2,3,4);
		Vector4ui b = new Vector4ui (5,6,7,8);
		
		Vector4ui c = Vector4ui.UnpackLow (a, b);
	
		if (c.X != 1)
			return 1;
		if (c.Y != 5)
			return 2;
		if (c.Z != 2)
			return 3;
		if (c.W != 6)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_xor () {
		Vector4ui a = new Vector4ui (1,2,3,4);
		Vector4ui b = new Vector4ui (7,5,3,1);
		
		Vector4ui c = a ^ b;
	
		if (c.X != 6)
			return 1;
		if (c.Y != 7)
			return 2;
		if (c.Z != 0)
			return 3;
		if (c.W != 5)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_or () {
		Vector4ui a = new Vector4ui (1,2,3,4);
		Vector4ui b = new Vector4ui (7,5,3,1);
		
		Vector4ui c = a | b;
	
		if (c.X != 7)
			return 1;
		if (c.Y != 7)
			return 2;
		if (c.Z != 3)
			return 3;
		if (c.W != 5)
			return 4;
		return 0;
	}
	public  static int test_0_vector4ui_and () {
		Vector4ui a = new Vector4ui (1,2,3,4);
		Vector4ui b = new Vector4ui (7,5,3,1);
		
		Vector4ui c = a & b;
	
		if (c.X != 1)
			return 1;
		if (c.Y != 0)
			return 2;
		if (c.Z != 3)
			return 3;
		if (c.W != 0)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_shr () {
		Vector4ui a = new Vector4ui (0xF0000000u,20,3,40);
		
		Vector4ui c = a >> 2;
	
		if (c.X != 0x3C000000)
			return 1;
		if (c.Y != 5)
			return 2;
		if (c.Z != 0)
			return 3;
		if (c.W != 10)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_shl () {
		Vector4ui a = new Vector4ui (10,20,3,40);
		
		Vector4ui c = a << 2;
	
		if (c.X != 40)
			return 1;
		if (c.Y != 80)
			return 2;
		if (c.Z != 12)
			return 3;
		if (c.W != 160)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_mul () {
		Vector4ui a = new Vector4ui (0x8888,20,3,40);
		Vector4ui b = new Vector4ui (0xFF00FF00u,2,3,4);
		
		Vector4ui c = a * b;
	
		if (c.X != 0xffff7800)
			return 1;
		if (c.Y != 40)
			return 2;
		if (c.Z != 9)
			return 3;
		if (c.W != 160)
			return 4;
		return 0;
	}
	public  static int test_0_vector4ui_sub () {
		Vector4ui a = new Vector4ui (1,20,3,40);
		Vector4ui b = new Vector4ui (0xFF00FF00u,2,3,4);
		
		Vector4ui c = a - b;
	
		if (c.X != 0xff0101)
			return 1;
		if (c.Y != 18)
			return 2;
		if (c.Z != 0)
			return 3;
		if (c.W != 36)
			return 4;
		return 0;
	}

	public  static int test_0_vector4ui_add () {
		Vector4ui a = new Vector4ui (0xFF00FF00u,2,3,4);
		Vector4ui b = new Vector4ui (0xFF00FF00u,2,3,4);
		
		Vector4ui c = a + b;
	
		if (c.X != 0xfe01fe00)
			return 1;
		if (c.Y != 4)
			return 2;
		if (c.Z != 6)
			return 3;
		if (c.W != 8)
			return 4;
		return 0;
	}


	static int test_0_vector4ui_accessors () {
		Vector4ui a = new Vector4ui (1,2,3,4);

		if (a.X != 1)
			return 1;
		if (a.Y != 2)
			return 2;
		if (a.Z != 3)
			return 3;
		if (a.W != 4)
			return 4;
		a.X = 10;
		a.Y = 20;
		a.Z = 30;
		a.W = 40;

		if (a.X != 10)
			return 5;
		if (a.Y != 20)
			return 6;
		if (a.Z != 30)
			return 7;
		if (a.W != 40)
			return 8;
		return 0;
	}

	static int test_0_vector8us_sub_sat () {
		Vector8us a = new Vector8us (0xF000,1,20,3,4,5,6,7);
		Vector8us b = new Vector8us (0xFF00,4,5,6,7,8,9,10);
		Vector8us c = Vector8us.SubWithSaturation (a, b);

		if (c.V0 != 0)
			return 1;
		if (c.V1 != 0)
			return 2;
		if (c.V2 != 15)
			return 3;
		if (c.V3 != 0)
			return 4;
		if (c.V4 != 0)
			return 5;
		if (c.V5 != 0)
			return 6;
		if (c.V6 != 0)
			return 7;
		if (c.V7 != 0)
			return 8;
		return 0;
	}

	static int test_0_vector8us_add_sat () {
		Vector8us a = new Vector8us (0xFF00,1,2,3,4,5,6,7);
		Vector8us b = new Vector8us (0xFF00,4,5,6,7,8,9,10);
		Vector8us c = Vector8us.AddWithSaturation (a, b);

		if (c.V0 != 0xFFFF)
			return 1;
		if (c.V1 != 5)
			return 2;
		if (c.V2 != 7)
			return 3;
		if (c.V3 != 9)
			return 4;
		if (c.V4 != 11)
			return 5;
		if (c.V5 != 13)
			return 6;
		if (c.V6 != 15)
			return 7;
		if (c.V7 != 17)
			return 8;
		return 0;
	}

	static int test_0_vector8us_unpack_low () {
		Vector8us a = new Vector8us (0,1,2,3,4,5,6,7);
		Vector8us b = new Vector8us (3,4,5,6,7,8,9,10);
		Vector8us c = Vector8us.UnpackLow (a, b);

		if (c.V0 != 0)
			return 1;
		if (c.V1 != 3)
			return 2;
		if (c.V2 != 1)
			return 3;
		if (c.V3 != 4)
			return 4;
		if (c.V4 != 2)
			return 5;
		if (c.V5 != 5)
			return 6;
		if (c.V6 != 3)
			return 7;
		if (c.V7 != 6)
			return 8;
		return 0;
	}


	static int test_0_vector8us_shift_left () {
		Vector8us a = new Vector8us (0xFF00,1,2,3,4,5,6,7);
		int amt = 2;
		Vector8us c = a << amt;
	
		if (c.V0 != 0xFC00)
			return 1;
		if (c.V1 != 4)
			return 2;
		if (c.V7 != 28)
			return 3;
		return 0;
	}
	
	static int test_0_vector8us_shift_right_arithmetic () {
		Vector8us a = new Vector8us (0xFF00,1,2,3,4,5,6,7);
		int amt = 2;
		Vector8us c = Vector8us.ShiftRightArithmetic (a, amt);
	
		if (c.V0 != 0xFFC0)
			return 1;
		if (c.V1 != 0)
			return 2;
		if (c.V7 != 1)
			return 3;
		return 0;
	}

	static int test_0_vector8us_shift_variable_offset () {
		int off = 2;
		Vector8us a = new Vector8us (0xF000,1,2,3,4,5,6,7);
		Vector8us b = a;
		Vector8us c = b >> off;
		a = b + b;

		if (c.V0 != 0x3C00)
			return 1;
		if (c.V1 != 0)
			return 2;
		if (c.V7 != 1)
			return 3;
		if (a.V1 != 2)
			return 4;
		if (a.V7 != 14)
			return 5;
		return 0;
	}
	
	
	static int test_0_vector8us_shift_operand_is_live_after_op () {
		Vector8us a = new Vector8us (0xF000,1,2,3,4,5,6,7);
		Vector8us b = a;
		Vector8us c = b >> 2;
		a = b + b;

		if (c.V0 != 0x3C00)
			return 1;
		if (c.V1 != 0)
			return 2;
		if (c.V7 != 1)
			return 3;
		if (a.V1 != 2)
			return 4;
		if (a.V7 != 14)
			return 5;
		return 0;
	}

	static int test_0_vector8us_shr_constant () {
		Vector8us a = new Vector8us (0xF000,1,2,3,4,5,6,7);
		Vector8us c = a >> 2;

		if (c.V0 != 0x3C00)
			return 1;
		if (c.V1 != 0)
			return 2;
		if (c.V7 != 1)
			return 3;
		return 0;
	}

	static int test_0_vector8us_mul () {
		Vector8us a = new Vector8us (0x0F00,4,5,6,7,8,9,10);
		Vector8us b = new Vector8us (0x0888,1,2,3,4,5,6,8);

		Vector8us c = a * b;
		if (c.V0 != 63488)
			return 1;
		if (c.V1 != 4)
			return 2;
		if (c.V7 != 80)
			return 3;
		return 0;
	}

	static int test_0_vector8us_add () {
		Vector8us a = new Vector8us (0xFF00,4,5,6,7,8,9,10);
		Vector8us b = new Vector8us (0x8888,1,2,3,4,5,6,8);

		Vector8us c = a + b;
		if (c.V0 != 34696)
			return 1;
		if (c.V1 != 5)
			return 2;
		if (c.V7 != 18)
			return 3;
		return 0;
	}


	static int test_0_vector8us_sub () {
		Vector8us a = new Vector8us (3,4,5,6,7,8,9,10);
		Vector8us b = new Vector8us (10,1,2,3,4,5,6,8);

		Vector8us c = a - b;

		if (c.V0 != 65529)
			return 1;
		if (c.V1 != 3)
			return 2;
		if (c.V7 != 2)
			return 3;
		return 0;
	}


	static int test_0_vector8us_accessors () {
		Vector8us a = new Vector8us (0,1,2,3,4,5,6,7);

		if (a.V0 != 0)
			return 1;
		if (a.V1 != 1)
			return 2;
		if (a.V2 != 2)
			return 3;
		if (a.V3 != 3)
			return 4;
		if (a.V4 != 4)
			return 5;
		if (a.V5 != 5)
			return 6;
		if (a.V6 != 6)
			return 7;
		if (a.V7 != 7)
			return 8;
		a.V0 = 10;
		a.V1 = 20;
		a.V2 = 30;
		a.V3 = 40;
		a.V4 = 50;
		a.V5 = 60;
		a.V6 = 70;
		a.V7 = 80;

		if (a.V0 != 10)
			return 17;
		if (a.V1 != 20)
			return 18;
		if (a.V2 != 30)
			return 19;
		if (a.V3 != 40)
			return 20;
		if (a.V4 != 50)
			return 21;
		if (a.V5 != 60)
			return 22;
		if (a.V6 != 70)
			return 23;
		if (a.V7 != 80)
			return 24;

		return 0;
	}


	static int test_0_vector16b_unpack_high () {
		Vector16b a = new Vector16b (0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
		Vector16b b = new Vector16b (9,10,11,12,13,14,15,0,1,2,3,4,5,6,7,8);
		Vector16b c = Vector16b.UnpackHigh (a, b);

		if (c.V0 != 8)
			return 1;
		if (c.V1 != 1)
			return 2;
		if (c.V2 != 9)
			return 3;
		if (c.V3 != 2)
			return 4;
		if (c.V4 != 10)
			return 5;
		if (c.V5 != 3)
			return 6;
		if (c.V14 != 15)
			return 7;
		if (c.V15 != 8)
			return 8;
		return 0;
	}

	static int test_0_vector16b_unpack_low () {
		Vector16b a = new Vector16b (0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
		Vector16b b = new Vector16b (9,10,11,12,13,14,15,0,1,2,3,4,5,6,7,8);
		Vector16b c = Vector16b.UnpackLow (a, b);

		if (c.V0 != 0)
			return 1;
		if (c.V1 != 9)
			return 2;
		if (c.V2 != 1)
			return 3;
		if (c.V3 != 10)
			return 4;
		if (c.V4 != 2)
			return 5;
		if (c.V5 != 11)
			return 6;
		if (c.V14 != 7)
			return 7;
		if (c.V15 != 0)
			return 8;
		return 0;
	}

	static int test_0_vector16b_sar () {
		Vector16b a = new Vector16b (0xF0,20,3,40,0,0,0,0,0,0,0,0,0,0,0,0);
		
		Vector16b c = Vector16b.ShiftRightArithmetic (a, 2);
		if (c.V0 != 0xFC)
			return 1;
		if (c.V1 != 5)
			return 1;
		if (c.V2 != 0)
			return 2;
		if (c.V3 != 10)
			return 3;
		return 0;
	}

	static int test_0_vector16b_sub_sat () {
		Vector16b a = new Vector16b (100,10,11,12,13,14,15,0,1,2,3,4,5,6,7,8);
		Vector16b b = new Vector16b (200,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
		Vector16b c = Vector16b.SubWithSaturation (a, b);

		if (c.V0 != 0)
			return 1;
		if (c.V1 != 9)
			return 2;
		if (c.V15 != 0)
			return 3;
		return 0;
	}
	
	static int test_0_vector16b_add_sat () {
		Vector16b a = new Vector16b (200,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
		Vector16b b = new Vector16b (200,10,11,12,13,14,15,0,1,2,3,4,5,6,7,8);
		Vector16b c = Vector16b.AddWithSaturation (a, b);

		if (c.V0 != 255)
			return 1;
		if (c.V1 != 11)
			return 2;
		if (c.V15 != 23)
			return 3;
		return 0;
	}

	static int test_0_vector16b_add_ovf () {
		Vector16b a = new Vector16b (200,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
		Vector16b b = new Vector16b (200,10,11,12,13,14,15,0,1,2,3,4,5,6,7,8);
		Vector16b c = a + b;

		if (c.V0 != 144)
			return 1;
		if (c.V1 != 11)
			return 2;
		if (c.V15 != 23)
			return 3;
		return 0;
	}

	static int test_0_vector16b_accessors () {
		Vector16b a = new Vector16b (0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);

		if (a.V0 != 0)
			return 1;
		if (a.V1 != 1)
			return 2;
		if (a.V2 != 2)
			return 3;
		if (a.V3 != 3)
			return 4;
		if (a.V4 != 4)
			return 5;
		if (a.V5 != 5)
			return 6;
		if (a.V6 != 6)
			return 7;
		if (a.V7 != 7)
			return 8;
		if (a.V8 != 8)
			return 9;
		if (a.V9 != 9)
			return 10;
		if (a.V10 != 10)
			return 11;
		if (a.V11 != 11)
			return 12;
		if (a.V12 != 12)
			return 13;
		if (a.V13 != 13)
			return 14;
		if (a.V14 != 14)
			return 15;
		if (a.V15 != 15)
			return 16;

		a.V0 = 10;
		a.V1 = 20;
		a.V2 = 30;
		a.V3 = 40;
		a.V4 = 50;
		a.V5 = 60;
		a.V6 = 70;
		a.V7 = 80;
		a.V8 = 90;
		a.V9 = 100;
		a.V10 = 110;
		a.V11 = 120;
		a.V12 = 130;
		a.V13 = 140;
		a.V14 = 150;
		a.V15 = 160;

		if (a.V0 != 10)
			return 17;
		if (a.V1 != 20)
			return 18;
		if (a.V2 != 30)
			return 19;
		if (a.V3 != 40)
			return 20;
		if (a.V4 != 50)
			return 21;
		if (a.V5 != 60)
			return 22;
		if (a.V6 != 70)
			return 23;
		if (a.V7 != 80)
			return 24;
		if (a.V8 != 90)
			return 25;
		if (a.V9 != 100)
			return 26;
		if (a.V10 != 110)
			return 27;
		if (a.V11 != 120)
			return 28;
		if (a.V12 != 130)
			return 29;
		if (a.V13 != 140)
			return 30;
		if (a.V14 != 150)
			return 31;
		if (a.V15 != 160)
			return 32;
		return 0;
	}

	public static int test_0_accessors () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		if (a.X != 1f)
			return 1;
		if (a.Y != 2f)
			return 2;
		if (a.Z != 3f)
			return 3;
		if (a.W != 4f)
			return 4;
		return 0;
	}

	public static int test_0_packed_add_with_stack_tmp () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		Vector4f b = new Vector4f (5, 6, 7, 8);
		Vector4f c = new Vector4f (-1, -3, -4, -5);
		Vector4f d = a + b + c;
		if (d.X != 5f)
			return 1;
		if (d.Y != 5f)
			return 2;
		if (d.Z != 6f)
			return 3;
		if (d.W != 7f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_add () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		Vector4f b = new Vector4f (5, 6, 7, 8);
		Vector4f c;
		c = a + b;
		if (c.X != 6f)
			return 1;
		if (c.Y != 8f)
			return 2;
		if (c.Z != 10f)
			return 3;
		if (c.W != 12f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_sub () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		Vector4f b = new Vector4f (5, 6, 7, 8);
		Vector4f c = b - a;
		if (c.X != 4f)
			return 1;
		if (c.Y != 4f)
			return 2;
		if (c.Z != 4f)
			return 3;
		if (c.W != 4f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_mul () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		Vector4f b = new Vector4f (5, 6, 7, 8);
		Vector4f c = b * a;
		if (c.X != 5f)
			return 1;
		if (c.Y != 12f)
			return 2;
		if (c.Z != 21f)
			return 3;
		if (c.W != 32f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_div () {
		Vector4f a = new Vector4f (2, 2, 3, 4);
		Vector4f b = new Vector4f (20, 10, 33, 12);
		Vector4f c = b / a;
		if (c.X != 10f)
			return 1;
		if (c.Y != 5f)
			return 2;
		if (c.Z != 11f)
			return 3;
		if (c.W != 3f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_sqrt () {
		Vector4f a = new Vector4f (16, 4, 9, 25);
		a = Vector4f.Sqrt (a);
		if (a.X != 4f)
			return 1;
		if (a.Y != 2f)
			return 2;
		if (a.Z != 3f)
			return 3;
		if (a.W != 5f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_invsqrt () {
		Vector4f a = new Vector4f (16, 4, 100, 25);
		//this function has VERY low precision
		a = Vector4f.InvSqrt (a);
		if (a.X < (1/4f - 0.01f) || a.X > (1/4f + 0.01f))
			return 1;
		if (a.Y < (1/2f - 0.01f) || a.Y > (1/2f + 0.01f))
			return 2;
		if (a.Z < (1/10f - 0.01f) || a.Z > (1/10f + 0.01f))
			return 3;
		if (a.W < (1/5f - 0.01f) || a.W > (1/5f + 0.01f))
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_min () {
		Vector4f a = new Vector4f (16, -4, 9, 25);
		Vector4f b = new Vector4f (5, 3, 9, 0);
		Vector4f c = Vector4f.Min (a, b);
		if (c.X != 5f)
			return 1;
		if (c.Y != -4f)
			return 2;
		if (c.Z != 9f)
			return 3;
		if (c.W != 0f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_max () {
		Vector4f a = new Vector4f (16, -4, 9, 25);
		Vector4f b = new Vector4f (5, 3, 9, 0);
		Vector4f c = Vector4f.Max (a, b);
		if (c.X != 16f)
			return 1;
		if (c.Y != 3f)
			return 2;
		if (c.Z != 9f)
			return 3;
		if (c.W != 25f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_hadd () {
		Vector4f a = new Vector4f (5, 5, 6, 6);
		Vector4f b = new Vector4f (7, 7, 8, 8);
		Vector4f c = Vector4f.HorizontalAdd (a, b);
		if (c.X != 10f)
			return 1;
		if (c.Y != 12f)
			return 2;
		if (c.Z != 14f)
			return 3;
		if (c.W != 16f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_hsub () {
		Vector4f a = new Vector4f (5, 2, 6, 1);
		Vector4f b = new Vector4f (7, 0, 8, 3);
		Vector4f c = Vector4f.HorizontalSub (a, b);
		if (c.X != 3f)
			return 1;
		if (c.Y != 5f)
			return 2;
		if (c.Z != 7f)
			return 3;
		if (c.W != 5f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_addsub () {
		Vector4f a = new Vector4f (5, 2, 6, 1);
		Vector4f b = new Vector4f (7, 0, 8, 3);
		Vector4f c = Vector4f.AddSub (a, b);
		if (c.X != -2f)
			return 1;
		if (c.Y != 2f)
			return 2;
		if (c.Z != -2f)
			return 3;
		if (c.W != 4f)
			return 4;
		return 0;
	}

	public static int test_0_simple_packed_shuffle () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		a = Vector4f.Shuffle(a, ShuffleSel.XFromY | ShuffleSel.YFromW | ShuffleSel.ZFromX | ShuffleSel.WFromZ);
		if (a.X != 2f)
			return 1;
		if (a.Y != 4f)
			return 2;
		if (a.Z != 1f)
			return 3;
		if (a.W != 3f)
			return 4;
		return 0;
	}

	public static int test_0_packed_shuffle_with_reg_pressure () {
		Vector4f v = new Vector4f (1, 2, 3, 4);
		Vector4f m0 = v + v, m1 = v - v, m2 = v * v, m3 = v + v + v;
		if (ff) v = v + v -v	;

		Vector4f r0 = Vector4f.Shuffle (v, ShuffleSel.XFromY | ShuffleSel.YFromW | ShuffleSel.ZFromX | ShuffleSel.WFromZ);
		Vector4f r1 = Vector4f.Shuffle (v, ShuffleSel.XFromY | ShuffleSel.YFromW | ShuffleSel.ZFromX | ShuffleSel.WFromZ);
		Vector4f x = Vector4f.Shuffle (v, ShuffleSel.XFromY | ShuffleSel.YFromW | ShuffleSel.ZFromX | ShuffleSel.WFromZ);
		Vector4f r2 = Vector4f.Shuffle (v, ShuffleSel.XFromY | ShuffleSel.YFromW | ShuffleSel.ZFromX | ShuffleSel.WFromZ);
		Vector4f r3 = Vector4f.Shuffle (v, ShuffleSel.XFromY | ShuffleSel.YFromW | ShuffleSel.ZFromX | ShuffleSel.WFromZ);
		Vector4f a = x;

		r0 = r0 * m0 + x;
		r1 = r1 * m1 + x;
		x = x - v + v;
		r2 = r2 * m2 + x;
		r3 = r3 * m3 + x;
		Vector4f result = r0 + r1 + r2 + r3;

		if (a.X != 2f)
			return 1;
		if (a.Y != 4f)
			return 2;
		if (a.Z != 1f)
			return 3;
		if (a.W != 3f)
			return 4;
		if (result.Y != result.Y)
			return 0;
		return 0;
	}
	
	public static int test_24_regs_pressure_a () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		Vector4f b = a + a;
		Vector4f c = b * a;
		Vector4f d = a - b;
		c = a + b + c + d;
		return (int)c.Z;
	}

	public static int test_54_regs_pressure_b () {
		Vector4f a = new Vector4f (1, 2, 3, 4);
		Vector4f b = a + a;
		Vector4f c = b - a;
		Vector4f d = c - a;
		Vector4f e = a + b + c;
		Vector4f f = d - b + a - c;
		Vector4f g = a - d * f - c + b;
		Vector4f h = a * b - c + e;
		Vector4f i = h - g - f - e - d - c - b - a;
		Vector4f j = a + b + c + d + e + f + g + h + i;
		return (int)j.Z;
	}

	static bool ff;
	public static int test_3_single_block_var_is_properly_promoted () {
		Vector4f a = new Vector4f (4, 5, 6, 7);
		if (ff)
			a = a - a;
		else {
			Vector4f b = new Vector4f (1, 2, 3, 4);
			Vector4f c = b;
			a = a - b;
			if (ff) {
				c = a;
				a = c;
			}
		}
		return (int)a.X;
	}

	static float float_val = 45f;

	public static int test_0_sse2_opt_and_simd_intrinsic_proper_regalloc () {
		Vector4f v = new Vector4f (1, 2, 3, 4);
		float f = float_val;
		int x = (int)f;
		if (v.X != 1f)
			return 1;
		if (x != 45f)
			return 2;
		return 0;
	}

	public static int Main () {
		return TestDriver.RunTests (typeof (SimdTests));
	}
}

