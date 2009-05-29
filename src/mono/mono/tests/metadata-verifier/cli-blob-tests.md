method-def-sig {
	assembly assembly-with-methods.exe

	#bad first byte
	#method zero is a default ctor
	#0 -> default 5 -> vararg

	#signature size, zero is invalid
	invalid offset blob.i (table-row (6 0) + 10) set-byte 0

	#cconv
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x26
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x27
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x28
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x29
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x2A
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x2B
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x2C
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x2D
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x2E
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-byte 0x2F

	#upper nimble flags 0x80 is invalid	
	invalid offset blob.i (table-row (6 0) + 10) + 1 set-bit 7

	#sig is too small to decode param count
	invalid offset blob.i (table-row (6 0) + 10) set-byte 1

	#sig is too small to decode return type
	invalid offset blob.i (table-row (6 0) + 10) set-byte 2

	#zero generic args
	#method 1 is generic
	#bytes: size cconv gen_param_count
	invalid offset blob.i (table-row (6 1) + 10) + 2 set-byte 0

	#set ret type to an invalid value
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x17
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x1A
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x21 #mono doesn't support internal type
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x40 #modifier
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x41 #sentinel
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x45 #pinner
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x50 #type
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x51 #boxed
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x52 #reserved
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x53 #field
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x54 #property
	invalid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x55 #enum

	#bad args
	#method 12 has sig void (int,int,int)
	#bytes: size cconv param_count void int32 int32 int32
	valid offset blob.i (table-row (6 12) + 10) + 4 set-byte 0x05
	valid offset blob.i (table-row (6 12) + 10) + 5 set-byte 0x06
	valid offset blob.i (table-row (6 12) + 10) + 6 set-byte 0x07

	#void
	invalid offset blob.i (table-row (6 12) + 10) + 5 set-byte 0x01

	#byref without anything after
	invalid offset blob.i (table-row (6 12) + 10) + 4 set-byte 0x10
	invalid offset blob.i (table-row (6 12) + 10) + 5 set-byte 0x10
	invalid offset blob.i (table-row (6 12) + 10) + 6 set-byte 0x10
}

#Test for stuff in the ret that can't be expressed with C#
method-def-ret-misc {
	assembly assembly-with-custommod.exe

	#method 0 has a modreq
	#bytes: size cconv param_count mod_req compressed_token
	invalid offset blob.i (table-row (6 0) + 10) + 4 set-byte 0x7C
	invalid offset blob.i (table-row (6 0) + 10) + 4 set-byte 0x07

	#switch modreq to modopt
	valid offset blob.i (table-row (6 0) + 10) + 3 set-byte 0x20

	#2 times byref
	#method 4 returns byref
	#bytes: size cconv param_count byref int32
	invalid offset blob.i (table-row (6 4) + 10) + 4 set-byte 0x10
	#byref of typedref
	invalid offset blob.i (table-row (6 4) + 10) + 4 set-byte 0x16

}

method-ref-sig {
	assembly assembly-with-signatures.exe

	#member ref 0 is has a vararg sig 
	#member ref 1 don't use vararg

	#2 sentinels
	#bytes: size cconv pcount void str obj obj obj obj ... i32 i32 i32
	invalid offset blob.i (table-row (0xA 0) + 4) + 10 set-byte 0x41
	invalid offset blob.i (table-row (0xA 0) + 4) + 11 set-byte 0x41

	#sentinel but not vararg
	invalid offset blob.i (table-row (0xA 0) + 4) + 1 set-byte 0
}

stand-alone-method-sig {
	assembly assembly-with-custommod.exe

	#standalone sig 0x3 points to an icall sig
	valid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x0
	valid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x1
	valid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x2
	valid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x3
	valid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x4
	valid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x5

	#sig is int32 (int32)
	#size cconv pcount(1) int32 int32 ->
	#size cconv gcount(1) pcount(0) int32
	#cannot have generics
	invalid offset blob.i (table-row (0x11 3)) + 1 set-byte 0x10,
			offset blob.i (table-row (0x11 3)) + 2 set-byte 1,
			offset blob.i (table-row (0x11 3)) + 3 set-byte 0
}

field-sig {
	assembly assembly-with-complex-type.exe

	#first byte must be 6
	invalid offset blob.i (table-row (4 0) + 4) + 1 set-byte 0x0
	invalid offset blob.i (table-row (4 0) + 4) + 1 set-byte 0x5
	invalid offset blob.i (table-row (4 0) + 4) + 1 set-byte 0x7
	invalid offset blob.i (table-row (4 0) + 4) + 1 set-byte 0x16
	invalid offset blob.i (table-row (4 0) + 4) + 1 set-byte 0x26

}

