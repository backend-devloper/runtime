#! /bin/sh

# Stack Size Tests
for OP in 'starg.s 0'
do
  ./make_stack_0_test.sh invalid "$OP"
done

for OP in 'stloc.0' 'stloc.s 0' 'stfld int32 Class::fld' pop ret
do
  ./make_stack_0_test.sh invalid "$OP"
done

for OP in add and 'box [mscorlib]System.Int32' 'brfalse branch_target' ceq cgt clt conv.i4 conv.r8 div dup 'ldfld int32 Class::fld' 'ldflda int32 Class::fld' mul not or rem shl shr sub xor
do
  ./make_stack_0_pop_test.sh "$OP"
done

for OP in add and ceq cgt clt div dup mul or rem shl shr sub xor 'stfld int32 Class::fld'
do
  ./make_stack_1_pop_test.sh "$OP" int32
done

# Table 2: Binary Numeric Operators
I=1
for OP in add div mul rem sub
do
  if [ "$OP" == "div" ] || [ "$OP" == "rem" ]; then
  	INIT="yes";
  else
  	INIT="no";
  fi
	
  ./make_bin_test.sh bin_num_op_32_${I} valid $OP int32 int32 "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_33_${I} valid $OP int32 'native int' "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_34_${I} valid $OP int64 int64 "ldc.i8 1" "${INIT}"
  ./make_bin_test.sh bin_num_op_35_${I} valid $OP 'native int' int32 "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_36_${I} valid $OP 'native int' 'native int' "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_37_${I} valid $OP float64 float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_38_${I} valid $OP float32 float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_39_${I} valid $OP float64 float32 "ldc.r4 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_40_${I} valid $OP float32 float32 "ldc.r4 0" "${INIT}"
  
  ./make_bin_test.sh bin_num_op_1_${I} unverifiable $OP int32 int64 "ldc.i8 1" "${INIT}"
  ./make_bin_test.sh bin_num_op_2_${I} unverifiable $OP int32 float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_3_${I} unverifiable $OP int32 object "ldnull" "${INIT}"

  ./make_bin_test.sh bin_num_op_4_${I} unverifiable $OP int64 int32 "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_5_${I} unverifiable $OP int64 'native int' "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_6_${I} unverifiable $OP int64 float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_7_${I} unverifiable $OP int64 'int64&' "ldnull" "${INIT}"
  ./make_bin_test.sh bin_num_op_8_${I} unverifiable $OP int64 object "ldnull" "${INIT}"

  ./make_bin_test.sh bin_num_op_9_${I} unverifiable $OP 'native int' int64 "ldc.i8 1" "${INIT}"
  ./make_bin_test.sh bin_num_op_10_${I} unverifiable $OP 'native int' float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_11_${I} unverifiable $OP 'native int' object "ldnull" "${INIT}"

  ./make_bin_test.sh bin_num_op_12_${I} unverifiable $OP float64 int32 "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_13_${I} unverifiable $OP float64 int64 "ldc.i8 1" "${INIT}"
  ./make_bin_test.sh bin_num_op_14_${I} unverifiable $OP float64 'native int' "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_15_${I} unverifiable $OP float64 'float64&' "ldnull" "${INIT}"
  ./make_bin_test.sh bin_num_op_16_${I} unverifiable $OP float64 object "ldnull" "${INIT}"

  ./make_bin_test.sh bin_num_op_17_${I} unverifiable $OP 'int64&' int64 "ldc.i8 1" "${INIT}"
  ./make_bin_test.sh bin_num_op_18_${I} unverifiable $OP 'float64&' float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_19_${I} unverifiable $OP 'object&' object "ldnull" "${INIT}"

  ./make_bin_test.sh bin_num_op_20_${I} unverifiable $OP object int32 "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_21_${I} unverifiable $OP object int64 "ldc.i8 1" "${INIT}"
  ./make_bin_test.sh bin_num_op_22_${I} unverifiable $OP object 'native int' "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_23_${I} unverifiable $OP object float64 "ldc.r8 0" "${INIT}"
  ./make_bin_test.sh bin_num_op_24_${I} unverifiable $OP object 'object&' "ldnull" "${INIT}"
  ./make_bin_test.sh bin_num_op_25_${I} unverifiable $OP object object "ldnull" "${INIT}"
  I=`expr $I + 1`
done

I=1
for OP in div mul rem sub
do
  ./make_bin_test.sh bin_num_op_26_${I} unverifiable $OP int32 'int32&'
  ./make_bin_test.sh bin_num_op_27_${I} unverifiable $OP 'native int' 'native int&'
  I=`expr $I + 1`
done

for OP in add
do
  ./make_bin_test.sh bin_num_op_26_${I} unverifiable $OP int32 'int32&'
  ./make_bin_test.sh bin_num_op_27_${I} unverifiable $OP 'native int' 'native int&'
  I=`expr $I + 1`
done

I=1
for OP in div mul rem
do
  if [ "$OP" == "div" ] || [ "$OP" == "div" ]; then
  	INIT="yes";
  else
  	INIT="no";
  fi
  ./make_bin_test.sh bin_num_op_28_${I} unverifiable $OP 'int32&' int32 "ldc.i4.1" "${INIT}"
  ./make_bin_test.sh bin_num_op_29_${I} unverifiable $OP 'native int&' 'native int' "ldc.i4.1" "${INIT}"
  I=`expr $I + 1`
done

for OP in add sub
do
  ./make_bin_test.sh bin_num_op_28_${I} unverifiable $OP 'int32&' int32
  ./make_bin_test.sh bin_num_op_29_${I} unverifiable $OP 'native int&' 'native int'
  I=`expr $I + 1`
done

I=1
for OP in div mul rem add
do
  if [ "$OP" == "div" ] || [ "$OP" == "div" ]; then
  	INIT="yes";
  else
  	INIT="no";
  fi
  ./make_bin_test.sh bin_num_op_30_${I} unverifiable $OP 'int32&' 'int32&' "ldnull" "${INIT}"
  I=`expr $I + 1`
done

for OP in sub
do
  ./make_bin_test.sh bin_num_op_30_${I} unverifiable $OP 'int32&' 'int32&'
  I=`expr $I + 1`
done

# Table 4: Binary Comparison or Branch Operations
I=1
for OP in ceq cgt clt
do
  ./make_bin_test.sh bin_comp_op_1_${I} unverifiable $OP int32 int64
  ./make_bin_test.sh bin_comp_op_2_${I} unverifiable $OP int32 float64
  ./make_bin_test.sh bin_comp_op_3_${I} unverifiable $OP int32 'int32&'
  ./make_bin_test.sh bin_comp_op_4_${I} unverifiable $OP int32 object

  ./make_bin_test.sh bin_comp_op_5_${I} unverifiable $OP int64 int32
  ./make_bin_test.sh bin_comp_op_6_${I} unverifiable $OP int64 'native int'
  ./make_bin_test.sh bin_comp_op_7_${I} unverifiable $OP int64 float64
  ./make_bin_test.sh bin_comp_op_8_${I} unverifiable $OP int64 'int64&'
  ./make_bin_test.sh bin_comp_op_9_${I} unverifiable $OP int64 object

  ./make_bin_test.sh bin_comp_op_10_${I} unverifiable $OP 'native int' int64
  ./make_bin_test.sh bin_comp_op_11_${I} unverifiable $OP 'native int' float64
  ./make_bin_test.sh bin_comp_op_12_${I} unverifiable $OP 'native int' object

  ./make_bin_test.sh bin_comp_op_13_${I} unverifiable $OP float64 int32
  ./make_bin_test.sh bin_comp_op_14_${I} unverifiable $OP float64 int64
  ./make_bin_test.sh bin_comp_op_15_${I} unverifiable $OP float64 'native int'
  ./make_bin_test.sh bin_comp_op_16_${I} unverifiable $OP float64 'float64&'
  ./make_bin_test.sh bin_comp_op_17_${I} unverifiable $OP float64 object

  ./make_bin_test.sh bin_comp_op_18_${I} unverifiable $OP 'int32&' int32
  ./make_bin_test.sh bin_comp_op_19_${I} unverifiable $OP 'int64&' int64
  ./make_bin_test.sh bin_comp_op_20_${I} unverifiable $OP 'float64&' float64
  ./make_bin_test.sh bin_comp_op_21_${I} unverifiable $OP 'object&' object

  ./make_bin_test.sh bin_comp_op_22_${I} unverifiable $OP object int32
  ./make_bin_test.sh bin_comp_op_23_${I} unverifiable $OP object int64
  ./make_bin_test.sh bin_comp_op_24_${I} unverifiable $OP object 'native int'
  ./make_bin_test.sh bin_comp_op_25_${I} unverifiable $OP object float64
  ./make_bin_test.sh bin_comp_op_26_${I} unverifiable $OP object 'object&'
  I=`expr $I + 1`
done

I=1
for OP in cgt clt
do
  ./make_bin_test.sh bin_comp_op_27_${I} unverifiable $OP 'native int' 'native int&'
  ./make_bin_test.sh bin_comp_op_28_${I} unverifiable $OP 'native int&' 'native int'
  ./make_bin_test.sh bin_comp_op_29_${I} unverifiable $OP object object
  I=`expr $I + 1`
done

for OP in ceq
do
  ./make_bin_test.sh bin_comp_op_27_${I} unverifiable $OP 'native int' 'native int&'
  ./make_bin_test.sh bin_comp_op_28_${I} unverifiable $OP 'native int&' 'native int'
  I=`expr $I + 1`
done

# Table 5: Integer Operations
I=1
for OP in and or xor
do
  ./make_bin_test.sh bin_int_op_1_${I} unverifiable "$OP" int32 int64
  ./make_bin_test.sh bin_int_op_2_${I} unverifiable "$OP" int32 float64
  ./make_bin_test.sh bin_int_op_3_${I} unverifiable "$OP" int32 'int32&'
  ./make_bin_test.sh bin_int_op_4_${I} unverifiable "$OP" int32 object

  ./make_bin_test.sh bin_int_op_5_${I} unverifiable "$OP" int64 int32
  ./make_bin_test.sh bin_int_op_6_${I} unverifiable "$OP" int64 'native int'
  ./make_bin_test.sh bin_int_op_7_${I} unverifiable "$OP" int64 float64
  ./make_bin_test.sh bin_int_op_8_${I} unverifiable "$OP" int64 'int64&'
  ./make_bin_test.sh bin_int_op_9_${I} unverifiable "$OP" int64 object

  ./make_bin_test.sh bin_int_op_10_${I} unverifiable "$OP" 'native int' int64
  ./make_bin_test.sh bin_int_op_11_${I} unverifiable "$OP" 'native int' float64
  ./make_bin_test.sh bin_int_op_12_${I} unverifiable "$OP" 'native int' 'native int&'
  ./make_bin_test.sh bin_int_op_13_${I} unverifiable "$OP" 'native int' object

  ./make_bin_test.sh bin_int_op_14_${I} unverifiable "$OP" float64 int32
  ./make_bin_test.sh bin_int_op_15_${I} unverifiable "$OP" float64 int64
  ./make_bin_test.sh bin_int_op_16_${I} unverifiable "$OP" float64 'native int'
  ./make_bin_test.sh bin_int_op_17_${I} unverifiable "$OP" float64 float64
  ./make_bin_test.sh bin_int_op_18_${I} unverifiable "$OP" float64 'int32&'
  ./make_bin_test.sh bin_int_op_19_${I} unverifiable "$OP" float64 object

  ./make_bin_test.sh bin_int_op_20_${I} unverifiable "$OP" 'int32&' int32
  ./make_bin_test.sh bin_int_op_21_${I} unverifiable "$OP" 'int64&' int64
  ./make_bin_test.sh bin_int_op_22_${I} unverifiable "$OP" 'native int&' 'native int'
  ./make_bin_test.sh bin_int_op_23_${I} unverifiable "$OP" 'float64&' float64
  ./make_bin_test.sh bin_int_op_24_${I} unverifiable "$OP" 'int32&' 'int32&'
  ./make_bin_test.sh bin_int_op_25_${I} unverifiable "$OP" 'float64&' object

  ./make_bin_test.sh bin_int_op_26_${I} unverifiable "$OP" object int32
  ./make_bin_test.sh bin_int_op_27_${I} unverifiable "$OP" object int64
  ./make_bin_test.sh bin_int_op_28_${I} unverifiable "$OP" object 'native int'
  ./make_bin_test.sh bin_int_op_29_${I} unverifiable "$OP" object float64
  ./make_bin_test.sh bin_int_op_30_${I} unverifiable "$OP" object 'int32&'
  ./make_bin_test.sh bin_int_op_31_${I} unverifiable "$OP" object object
  I=`expr $I + 1`
done

I=1
for TYPE in bool int8 int16 char int32 int64 'native int' 'class Int8Enum'
do
  ./make_unary_test.sh not_${I} valid "not\n\tpop" "$TYPE"
  I=`expr $I + 1`
done

for TYPE in 'int32&' object "class MyStruct" typedref "method int32 *(int32)" 'class Template\`1<int32>' float32 float64
do
  ./make_unary_test.sh not_${I} unverifiable "not\n\tpop" "$TYPE"
  I=`expr $I + 1`
done

I=1
for TYPE in bool int8 int16 char int32 int64 'native int' 'class Int8Enum' float32 float64
do
  ./make_unary_test.sh neg_${I} valid "neg\n\tpop" "$TYPE"
  I=`expr $I + 1`
done

for TYPE in 'int32&' object "class MyStruct" typedref "method int32 *(int32)" 'class Template\`1<int32>'
do
  ./make_unary_test.sh neg_${I} unverifiable "neg\n\tpop" "$TYPE"
  I=`expr $I + 1`
done


# Table 6: Shift Operators
I=1
for OP in shl shr
do
  ./make_bin_test.sh shift_op_1_${I} unverifiable $OP int32 int64
  ./make_bin_test.sh shift_op_2_${I} unverifiable $OP int32 float64
  ./make_bin_test.sh shift_op_3_${I} unverifiable $OP int32 'int32&'
  ./make_bin_test.sh shift_op_4_${I} unverifiable $OP int32 object

  ./make_bin_test.sh shift_op_5_${I} unverifiable $OP int64 int64
  ./make_bin_test.sh shift_op_6_${I} unverifiable $OP int64 float64
  ./make_bin_test.sh shift_op_7_${I} unverifiable $OP int64 'int32&'
  ./make_bin_test.sh shift_op_8_${I} unverifiable $OP int64 object

  ./make_bin_test.sh shift_op_9_${I} unverifiable $OP 'native int' int64
  ./make_bin_test.sh shift_op_10_${I} unverifiable $OP 'native int' float64
  ./make_bin_test.sh shift_op_11_${I} unverifiable $OP 'native int' 'native int&'
  ./make_bin_test.sh shift_op_12_${I} unverifiable $OP 'native int' object

  ./make_bin_test.sh shift_op_13_${I} unverifiable $OP float64 int32
  ./make_bin_test.sh shift_op_14_${I} unverifiable $OP float64 int64
  ./make_bin_test.sh shift_op_15_${I} unverifiable $OP float64 'native int'
  ./make_bin_test.sh shift_op_16_${I} unverifiable $OP float64 float64
  ./make_bin_test.sh shift_op_17_${I} unverifiable $OP float64 'int32&'
  ./make_bin_test.sh shift_op_18_${I} unverifiable $OP float64 object

  ./make_bin_test.sh shift_op_19_${I} unverifiable $OP 'int32&' int32
  ./make_bin_test.sh shift_op_20_${I} unverifiable $OP 'int64&' int64
  ./make_bin_test.sh shift_op_21_${I} unverifiable $OP 'native int&' 'native int'
  ./make_bin_test.sh shift_op_22_${I} unverifiable $OP 'float64&' float64
  ./make_bin_test.sh shift_op_23_${I} unverifiable $OP 'int32&' 'int32&'
  ./make_bin_test.sh shift_op_24_${I} unverifiable $OP 'float64&' object

  ./make_bin_test.sh shift_op_25_${I} unverifiable $OP object int32
  ./make_bin_test.sh shift_op_26_${I} unverifiable $OP object int64
  ./make_bin_test.sh shift_op_27_${I} unverifiable $OP object 'native int'
  ./make_bin_test.sh shift_op_28_${I} unverifiable $OP object float64
  ./make_bin_test.sh shift_op_29_${I} unverifiable $OP object 'int32&'
  ./make_bin_test.sh shift_op_30_${I} unverifiable $OP object object
  I=`expr $I + 1`
done

# Table 8: Conversion Operations
I=1
J=1
for OP in "conv.i1\n\tpop" "conv.i2\n\tpop" "conv.i4\n\tpop" "conv.i8\n\tpop" "conv.r4\n\tpop" "conv.r8\n\tpop" "conv.u1\n\tpop" "conv.u2\n\tpop" "conv.u4\n\tpop" "conv.u8\n\tpop" "conv.i\n\tpop" "conv.u\n\tpop" "conv.r.un\n\tpop" "conv.ovf.i1\n\tpop" "conv.ovf.i2\n\tpop" "conv.ovf.i4\n\tpop" "conv.ovf.i8\n\tpop" "conv.ovf.u1\n\tpop" "conv.ovf.u2\n\tpop" "conv.ovf.u4\n\tpop" "conv.ovf.u8\n\tpop" "conv.ovf.i\n\tpop"  "conv.ovf.u\n\tpop" "conv.ovf.i1.un\n\tpop" "conv.ovf.i2.un\n\tpop" "conv.ovf.i4.un\n\tpop" "conv.ovf.i8.un\n\tpop" "conv.ovf.u1.un\n\tpop" "conv.ovf.u2.un\n\tpop" "conv.ovf.u4.un\n\tpop" "conv.ovf.u8.un\n\tpop" "conv.ovf.i.un\n\tpop"  "conv.ovf.u.un\n\tpop" 
do
  for TYPE in 'int8' 'bool' 'unsigned int8' 'int16' 'char' 'unsigned int16' 'int32' 'unsigned int32' 'int64' 'unsigned int64' 'float32' 'float64' 'native int' 'native unsigned int'
  do
    ./make_unary_test.sh conv_op_${J}_${I} valid $OP "$TYPE"
    I=`expr $I + 1`
  done

  for TYPE in 'object' 'string' 'class Class' 'valuetype MyStruct' 'int32[]' 'int32[,]' 'typedref' 'int32*' 'method int32 *(int32)' 'class Template`1<object>' 'int8&' 'bool&' 'unsigned int8&' 'int16&' 'char&' 'unsigned int16&' 'int32&' 'unsigned int32&' 'int64&' 'unsigned int64&' 'float32&' 'float64&' 'native int&' 'native unsigned int&' 'object&' 'string&' 'class Class&' 'valuetype MyStruct&' 'int32[]&' 'int32[,]&' 'typedref&'  'class Template`1<object>&'
  do
    ./make_unary_test.sh conv_op_${J}_${I} unverifiable $OP "$TYPE"
    I=`expr $I + 1`
  done
  J=`expr $J + 1`
  I=1
done

#local and argument store with invalid values lead to unverifiable code
I=1
for OP in stloc.0 "stloc.s 0" "starg 0" "starg.s 0"
do
  ./make_store_test.sh coercion_1_${I} unverifiable "$OP" int8 int64
  ./make_store_test.sh coercion_2_${I} unverifiable "$OP" int8 float64
  ./make_store_test.sh coercion_3_${I} unverifiable "$OP" int8 'int8&'
  ./make_store_test.sh coercion_4_${I} unverifiable "$OP" int8 object

  ./make_store_test.sh coercion_5_${I} unverifiable "$OP" 'unsigned int8' int64
  ./make_store_test.sh coercion_6_${I} unverifiable "$OP" 'unsigned int8' float64
  ./make_store_test.sh coercion_7_${I} unverifiable "$OP" 'unsigned int8' 'unsigned int8&'
  ./make_store_test.sh coercion_8_${I} unverifiable "$OP" 'unsigned int8' object

  ./make_store_test.sh coercion_9_${I} unverifiable "$OP" bool int64
  ./make_store_test.sh coercion_10_${I} unverifiable "$OP" bool float64
  ./make_store_test.sh coercion_11_${I} unverifiable "$OP" bool 'bool&'
  ./make_store_test.sh coercion_12_${I} unverifiable "$OP" bool object

  ./make_store_test.sh coercion_13_${I} unverifiable "$OP" int16 int64
  ./make_store_test.sh coercion_14_${I} unverifiable "$OP" int16 float64
  ./make_store_test.sh coercion_15_${I} unverifiable "$OP" int16 'int16&'
  ./make_store_test.sh coercion_16_${I} unverifiable "$OP" int16 object
  
  ./make_store_test.sh coercion_17_${I} unverifiable "$OP" 'unsigned int16' int64
  ./make_store_test.sh coercion_18_${I} unverifiable "$OP" 'unsigned int16' float64
  ./make_store_test.sh coercion_19_${I} unverifiable "$OP" 'unsigned int16' 'unsigned int16&'
  ./make_store_test.sh coercion_20_${I} unverifiable "$OP" 'unsigned int16' object
  
  ./make_store_test.sh coercion_21_${I} unverifiable "$OP" char int64
  ./make_store_test.sh coercion_22_${I} unverifiable "$OP" char float64
  ./make_store_test.sh coercion_23_${I} unverifiable "$OP" char 'char&'
  ./make_store_test.sh coercion_24_${I} unverifiable "$OP" char object
  
  ./make_store_test.sh coercion_25_${I} unverifiable "$OP" int32 int64
  ./make_store_test.sh coercion_26_${I} unverifiable "$OP" int32 float64
  ./make_store_test.sh coercion_27_${I} unverifiable "$OP" int32 'int32&'
  ./make_store_test.sh coercion_28_${I} unverifiable "$OP" int32 object
  
  ./make_store_test.sh coercion_29_${I} unverifiable "$OP" 'unsigned int32' int64
  ./make_store_test.sh coercion_30_${I} unverifiable "$OP" 'unsigned int32' float64
  ./make_store_test.sh coercion_31_${I} unverifiable "$OP" 'unsigned int32' 'unsigned int32&'
  ./make_store_test.sh coercion_32_${I} unverifiable "$OP" 'unsigned int32' object
 
  ./make_store_test.sh coercion_33_${I} unverifiable "$OP" int64 int32
  ./make_store_test.sh coercion_34_${I} unverifiable "$OP" int64 'native int'
  ./make_store_test.sh coercion_35_${I} unverifiable "$OP" int64 float64
  ./make_store_test.sh coercion_36_${I} unverifiable "$OP" int64 'int64&'
  ./make_store_test.sh coercion_37_${I} unverifiable "$OP" int64 object
  
  ./make_store_test.sh coercion_38_${I} unverifiable "$OP" 'unsigned int64' int32
  ./make_store_test.sh coercion_39_${I} unverifiable "$OP" 'unsigned int64' 'native int'
  ./make_store_test.sh coercion_40_${I} unverifiable "$OP" 'unsigned int64' float64
  ./make_store_test.sh coercion_41_${I} unverifiable "$OP" 'unsigned int64' 'unsigned int64&'
  ./make_store_test.sh coercion_42_${I} unverifiable "$OP" 'unsigned int64' object
  
  ./make_store_test.sh coercion_43_${I} unverifiable "$OP" 'native int' int64
  ./make_store_test.sh coercion_44_${I} unverifiable "$OP" 'native int' float64
  ./make_store_test.sh coercion_45_${I} unverifiable "$OP" 'native int' 'native int&'
  ./make_store_test.sh coercion_46_${I} unverifiable "$OP" 'native int' object
  
  ./make_store_test.sh coercion_47_${I} unverifiable "$OP" 'native unsigned int' int64
  ./make_store_test.sh coercion_48_${I} unverifiable "$OP" 'native unsigned int' float64
  ./make_store_test.sh coercion_49_${I} unverifiable "$OP" 'native unsigned int' 'native unsigned int&'
  ./make_store_test.sh coercion_50_${I} unverifiable "$OP" 'native unsigned int' object
  
  ./make_store_test.sh coercion_51_${I} unverifiable "$OP" float32 int32
  ./make_store_test.sh coercion_52_${I} unverifiable "$OP" float32 'native int'
  ./make_store_test.sh coercion_53_${I} unverifiable "$OP" float32 int64
  ./make_store_test.sh coercion_54_${I} unverifiable "$OP" float32 'float32&'
  ./make_store_test.sh coercion_55_${I} unverifiable "$OP" float32 object
  
  ./make_store_test.sh coercion_56_${I} unverifiable "$OP" float64 int32
  ./make_store_test.sh coercion_57_${I} unverifiable "$OP" float64 'native int'
  ./make_store_test.sh coercion_58_${I} unverifiable "$OP" float64 int64
  ./make_store_test.sh coercion_59_${I} unverifiable "$OP" float64 'float64&'
  ./make_store_test.sh coercion_60_${I} unverifiable "$OP" float64 object

  ./make_store_test.sh coercion_61_${I} unverifiable "$OP" object int32
  ./make_store_test.sh coercion_62_${I} unverifiable "$OP" object 'native int'
  ./make_store_test.sh coercion_63_${I} unverifiable "$OP" object int64
  ./make_store_test.sh coercion_64_${I} unverifiable "$OP" object float64
  ./make_store_test.sh coercion_65_${I} unverifiable "$OP" object 'object&'
  
  ./make_store_test.sh coercion_66_${I} unverifiable "$OP" 'class ValueType' int32
  ./make_store_test.sh coercion_67_${I} unverifiable "$OP" 'class ValueType' 'native int'
  ./make_store_test.sh coercion_68_${I} unverifiable "$OP" 'class ValueType' int64
  ./make_store_test.sh coercion_69_${I} unverifiable "$OP" 'class ValueType' float64
  ./make_store_test.sh coercion_70_${I} unverifiable "$OP" 'class ValueType' 'class ValueType&'
  ./make_store_test.sh coercion_71_${I} unverifiable "$OP" 'class ValueType' object
  
  ./make_store_test.sh coercion_72_${I} unverifiable "$OP" 'int32&' int32
  ./make_store_test.sh coercion_73_${I} unverifiable "$OP" 'int32&' 'native int'
  ./make_store_test.sh coercion_74_${I} unverifiable "$OP" 'int32&' int64
  ./make_store_test.sh coercion_75_${I} unverifiable "$OP" 'int32&' float64
  ./make_store_test.sh coercion_76_${I} unverifiable "$OP" 'int32&' object
  
  ./make_store_test.sh coercion_77_${I} unverifiable "$OP" typedref int32
  ./make_store_test.sh coercion_78_${I} unverifiable "$OP" typedref 'native int'
  ./make_store_test.sh coercion_89_${I} unverifiable "$OP" typedref int64
  ./make_store_test.sh coercion_80_${I} unverifiable "$OP" typedref float64
  ./make_store_test.sh coercion_81_${I} unverifiable "$OP" typedref 'typedref&'
  ./make_store_test.sh coercion_82_${I} unverifiable "$OP" typedref object
  I=`expr $I + 1`
done

function fix () {
	if [ "$3" != "" ]; then
		A=$3;
	elif [ "$2" != "" ]; then
		A=$2;
	else
		A=$1;
	fi

	if [ "$A" == "bool&" ]; then
		A="int8&";
	elif [ "$A" == "char&" ]; then
		A="int16&";
	fi

	echo "$A";
}

#Tests related to storing reference types on other reference types
I=1
for OP in stloc.0 "stloc.s 0" "starg 0" "starg.s 0"
do
	for TYPE1 in 'native int&' 'native unsigned int&'
	do
		for TYPE2 in 'int8&' 'unsigned int8&' 'bool&' 'int16&' 'unsigned int16&' 'char&' 'int32&' 'unsigned int32&' 'int64&' 'unsigned int64&' 'float32&' 'float64&' 'native int&' 'native unsigned int&'
		do
			TA="$(fix $TYPE1)"
			TB="$(fix $TYPE2)"
			if [ "$TA" == "$TB" ]; then
				./make_store_test.sh ref_coercion_${I} valid "$OP" "$TYPE1" "$TYPE2"
			elif [ "$TA" == "int32&" ] && [ "$TB" == "int&" ]; then
				./make_store_test.sh ref_coercion_${I} valid "$OP" "$TYPE1" "$TYPE2"
			elif [ "$TA" == "int&" ] && [ "$TB" == "int32&" ]; then
				./make_store_test.sh ref_coercion_${I} valid "$OP" "$TYPE1" "$TYPE2"
			else
				./make_store_test.sh ref_coercion_${I} unverifiable "$OP" "$TYPE1" "$TYPE2"
			fi
			I=`expr $I + 1`
		done
	done
done

I=1
for OP in stloc.0 "stloc.s 0" "starg 0" "starg.s 0"
do
	for TYPE1 in 'int8&' 'unsigned int8&' 'bool&' 'int16&' 'unsigned int16&' 'char&' 'int32&' 'unsigned int32&' 'int64&' 'unsigned int64&' 'float32&' 'float64&'
	do
		for TYPE2 in 'int8&' 'unsigned int8&' 'bool&' 'int16&' 'unsigned int16&' 'char&' 'int32&' 'unsigned int32&' 'int64&' 'unsigned int64&' 'float32&' 'float64&'
		do
			TA="$(fix $TYPE1)"
			TB="$(fix $TYPE2)"
			if [ "$TA" == "$TB" ]; then
				./make_store_test.sh ref_coercion_${I} valid "$OP" "$TYPE1" "$TYPE2"
			else
				./make_store_test.sh ref_coercion_${I} unverifiable "$OP" "$TYPE1" "$TYPE2"
			fi
			I=`expr $I + 1`
		done
	done
done

for OP in stloc.0 "stloc.s 0" "starg 0" "starg.s 0"
do
	for TYPE1 in 'class ClassA&' 'class ClassB&' 'class InterfaceA&' 'class InterfaceB&' 'class ValueType&'
	do
		for TYPE2 in 'class ClassA&' 'class ClassB&' 'class InterfaceA&' 'class InterfaceB&' 'class ValueType&'
		do
			if [ "$TYPE1" == "$TYPE2" ]; then
				./make_store_test.sh ref_coercion_${I} valid "$OP" "$TYPE1" "$TYPE2"
			else
				./make_store_test.sh ref_coercion_${I} unverifiable "$OP" "$TYPE1" "$TYPE2"
			fi
			I=`expr $I + 1`
		done
	done
done

#Field store parameter compatibility leads to invalid code
#Calling method with diferent verification types on stack lead to invalid code
I=1
for OP in "stfld TYPE1 Class::fld" "stsfld TYPE1 Class::sfld\n\tpop"  "call void Class::Method(TYPE1)"
do
  ./make_obj_store_test.sh obj_coercion_1_${I} unverifiable "$OP" int8 int64
  ./make_obj_store_test.sh obj_coercion_2_${I} unverifiable "$OP" int8 float64
  ./make_obj_store_test.sh obj_coercion_3_${I} unverifiable "$OP" int8 'int8&'
  ./make_obj_store_test.sh obj_coercion_4_${I} unverifiable "$OP" int8 object

  ./make_obj_store_test.sh obj_coercion_5_${I} unverifiable "$OP" 'unsigned int8' int64
  ./make_obj_store_test.sh obj_coercion_6_${I} unverifiable "$OP" 'unsigned int8' float64
  ./make_obj_store_test.sh obj_coercion_7_${I} unverifiable "$OP" 'unsigned int8' 'unsigned int8&'
  ./make_obj_store_test.sh obj_coercion_8_${I} unverifiable "$OP" 'unsigned int8' object

  ./make_obj_store_test.sh obj_coercion_9_${I} unverifiable "$OP" bool int64
  ./make_obj_store_test.sh obj_coercion_10_${I} unverifiable "$OP" bool float64
  ./make_obj_store_test.sh obj_coercion_11_${I} unverifiable "$OP" bool 'bool&'
  ./make_obj_store_test.sh obj_coercion_12_${I} unverifiable "$OP" bool object

  ./make_obj_store_test.sh obj_coercion_13_${I} unverifiable "$OP" int16 int64
  ./make_obj_store_test.sh obj_coercion_14_${I} unverifiable "$OP" int16 float64
  ./make_obj_store_test.sh obj_coercion_15_${I} unverifiable "$OP" int16 'int16&'
  ./make_obj_store_test.sh obj_coercion_16_${I} unverifiable "$OP" int16 object
  
  ./make_obj_store_test.sh obj_coercion_17_${I} unverifiable "$OP" 'unsigned int16' int64
  ./make_obj_store_test.sh obj_coercion_18_${I} unverifiable "$OP" 'unsigned int16' float64
  ./make_obj_store_test.sh obj_coercion_19_${I} unverifiable "$OP" 'unsigned int16' 'unsigned int16&'
  ./make_obj_store_test.sh obj_coercion_20_${I} unverifiable "$OP" 'unsigned int16' object
  
  ./make_obj_store_test.sh obj_coercion_21_${I} unverifiable "$OP" char int64
  ./make_obj_store_test.sh obj_coercion_22_${I} unverifiable "$OP" char float64
  ./make_obj_store_test.sh obj_coercion_23_${I} unverifiable "$OP" char 'char&'
  ./make_obj_store_test.sh obj_coercion_24_${I} unverifiable "$OP" char object
  
  ./make_obj_store_test.sh obj_coercion_25_${I} unverifiable "$OP" int32 int64
  ./make_obj_store_test.sh obj_coercion_26_${I} unverifiable "$OP" int32 float64
  ./make_obj_store_test.sh obj_coercion_27_${I} unverifiable "$OP" int32 'int32&'
  ./make_obj_store_test.sh obj_coercion_28_${I} unverifiable "$OP" int32 object
  
  ./make_obj_store_test.sh obj_coercion_29_${I} unverifiable "$OP" 'unsigned int32' int64
  ./make_obj_store_test.sh obj_coercion_30_${I} unverifiable "$OP" 'unsigned int32' float64
  ./make_obj_store_test.sh obj_coercion_31_${I} unverifiable "$OP" 'unsigned int32' 'unsigned int32&'
  ./make_obj_store_test.sh obj_coercion_32_${I} unverifiable "$OP" 'unsigned int32' object
 
  ./make_obj_store_test.sh obj_coercion_33_${I} unverifiable "$OP" int64 int32
  ./make_obj_store_test.sh obj_coercion_34_${I} unverifiable "$OP" int64 'native int'
  ./make_obj_store_test.sh obj_coercion_35_${I} unverifiable "$OP" int64 float64
  ./make_obj_store_test.sh obj_coercion_36_${I} unverifiable "$OP" int64 'int64&'
  ./make_obj_store_test.sh obj_coercion_37_${I} unverifiable "$OP" int64 object
  
  ./make_obj_store_test.sh obj_coercion_38_${I} unverifiable "$OP" 'unsigned int64' int32
  ./make_obj_store_test.sh obj_coercion_39_${I} unverifiable "$OP" 'unsigned int64' 'native int'
  ./make_obj_store_test.sh obj_coercion_40_${I} unverifiable "$OP" 'unsigned int64' float64
  ./make_obj_store_test.sh obj_coercion_41_${I} unverifiable "$OP" 'unsigned int64' 'unsigned int64&'
  ./make_obj_store_test.sh obj_coercion_42_${I} unverifiable "$OP" 'unsigned int64' object
  
  ./make_obj_store_test.sh obj_coercion_43_${I} unverifiable "$OP" 'native int' int64
  ./make_obj_store_test.sh obj_coercion_44_${I} unverifiable "$OP" 'native int' float64
  ./make_obj_store_test.sh obj_coercion_45_${I} unverifiable "$OP" 'native int' 'native int&'
  ./make_obj_store_test.sh obj_coercion_46_${I} unverifiable "$OP" 'native int' object
  
  ./make_obj_store_test.sh obj_coercion_47_${I} unverifiable "$OP" 'native unsigned int' int64
  ./make_obj_store_test.sh obj_coercion_48_${I} unverifiable "$OP" 'native unsigned int' float64
  ./make_obj_store_test.sh obj_coercion_49_${I} unverifiable "$OP" 'native unsigned int' 'native unsigned int&'
  ./make_obj_store_test.sh obj_coercion_50_${I} unverifiable "$OP" 'native unsigned int' object
  
  ./make_obj_store_test.sh obj_coercion_51_${I} unverifiable "$OP" float32 int32
  ./make_obj_store_test.sh obj_coercion_52_${I} unverifiable "$OP" float32 'native int'
  ./make_obj_store_test.sh obj_coercion_53_${I} unverifiable "$OP" float32 int64
  ./make_obj_store_test.sh obj_coercion_54_${I} unverifiable "$OP" float32 'float32&'
  ./make_obj_store_test.sh obj_coercion_55_${I} unverifiable "$OP" float32 object
  
  ./make_obj_store_test.sh obj_coercion_56_${I} unverifiable "$OP" float64 int32
  ./make_obj_store_test.sh obj_coercion_57_${I} unverifiable "$OP" float64 'native int'
  ./make_obj_store_test.sh obj_coercion_58_${I} unverifiable "$OP" float64 int64
  ./make_obj_store_test.sh obj_coercion_59_${I} unverifiable "$OP" float64 'float64&'
  ./make_obj_store_test.sh obj_coercion_60_${I} unverifiable "$OP" float64 object

  ./make_obj_store_test.sh obj_coercion_61_${I} unverifiable "$OP" object int32
  ./make_obj_store_test.sh obj_coercion_62_${I} unverifiable "$OP" object 'native int'
  ./make_obj_store_test.sh obj_coercion_63_${I} unverifiable "$OP" object int64
  ./make_obj_store_test.sh obj_coercion_64_${I} unverifiable "$OP" object float64
  ./make_obj_store_test.sh obj_coercion_65_${I} unverifiable "$OP" object 'object&'
  
  ./make_obj_store_test.sh obj_coercion_66_${I} unverifiable "$OP" 'class ValueType' int32
  ./make_obj_store_test.sh obj_coercion_67_${I} unverifiable "$OP" 'class ValueType' 'native int'
  ./make_obj_store_test.sh obj_coercion_68_${I} unverifiable "$OP" 'class ValueType' int64
  ./make_obj_store_test.sh obj_coercion_69_${I} unverifiable "$OP" 'class ValueType' float64
  ./make_obj_store_test.sh obj_coercion_70_${I} unverifiable "$OP" 'class ValueType' 'class ValueType&'
  ./make_obj_store_test.sh obj_coercion_71_${I} unverifiable "$OP" 'class ValueType' object
  
  
  #These tests don't test store error since one cannot have an 'int32&' field
  #They should exist in the structural tests session
  #./make_obj_store_test.sh obj_coercion_72_${I} invalid "$OP" 'int32&' int32
  #./make_obj_store_test.sh obj_coercion_73_${I} invalid "$OP" 'int32&' 'native int'
  #./make_obj_store_test.sh obj_coercion_74_${I} invalid "$OP" 'int32&' int64
  #./make_obj_store_test.sh obj_coercion_75_${I} invalid "$OP" 'int32&' float64
  #./make_obj_store_test.sh obj_coercion_76_${I} invalid "$OP" 'int32&' object
  
  ./make_obj_store_test.sh obj_coercion_77_${I} invalid "$OP" typedref int32
  ./make_obj_store_test.sh obj_coercion_78_${I} invalid "$OP" typedref 'native int'
  ./make_obj_store_test.sh obj_coercion_79_${I} invalid "$OP" typedref int64
  ./make_obj_store_test.sh obj_coercion_80_${I} invalid "$OP" typedref float64
  ./make_obj_store_test.sh obj_coercion_81_${I} invalid "$OP" typedref 'typedref&'
  ./make_obj_store_test.sh obj_coercion_82_${I} invalid "$OP" typedref object
  I=`expr $I + 1`
done

# 1.8.1.2.3 Verification type compatibility (Assignment compatibility)
I=1
for OP in stloc.0 "stloc.s 0" "starg.s 0"
do
  # ClassB not subtype of ClassA.
  ./make_store_test.sh assign_compat_1_${I} unverifiable "$OP" 'class ClassA' 'class ClassB'

  # ClassA not interface type.
  # FIXME: what was the purpoise of this test? on it's current for it is valid and not unverifiable
  ./make_store_test.sh assign_compat_3_${I} valid "$OP" object 'class ClassA'
  
  # Implementation of InterfaceB does not require the implementation of InterfaceA
  ./make_store_test.sh assign_compat_4_${I} unverifiable "$OP" 'class InterfaceA' 'class InterfaceB'

  # Array/vector.
  ./make_store_test.sh assign_compat_5_${I} unverifiable "$OP" 'string []' 'string[,]'

  # Vector/array.
  ./make_store_test.sh assign_compat_6_${I} unverifiable "$OP" 'string [,]' 'string[]'

  # Arrays with different rank.
  ./make_store_test.sh assign_compat_7_${I} unverifiable "$OP" 'string [,]' 'string[,,]'

  # Method pointers with different return types.
  ./make_store_test.sh assign_compat_8_${I} unverifiable "$OP" 'method int32 *(int32)' 'method float32 *(int32)'

  # Method pointers with different parameters.
  ./make_store_test.sh assign_compat_9_${I} unverifiable "$OP" 'method int32 *(float64)' 'method int32 *(int32)'

  # Method pointers with different calling conventions.
  ./make_store_test.sh assign_compat_10_${I} unverifiable "$OP" 'method vararg int32 *(int32)' 'method int32 *(int32)'

  # Method pointers with different calling conventions. (2)
  ./make_store_test.sh assign_compat_11_${I} unverifiable "$OP" 'method unmanaged fastcall int32 *(int32)' 'method int32 *(int32)'

  # Method pointers with different calling conventions. (3)
  ./make_store_test.sh assign_compat_12_${I} unverifiable "$OP" 'method unmanaged fastcall int32 *(int32)' 'method unmanaged stdcall int32 *(int32)'
  I=`expr $I + 1`
done

for OP in "stfld TYPE1 Class::fld" "stsfld TYPE1 Class::sfld\n\tpop"  "call void Class::Method(TYPE1)"
do
  # ClassB not subtype of ClassA.
  ./make_obj_store_test.sh assign_compat_1_${I} unverifiable "$OP" 'class ClassA' 'class ClassB'

  # object not subtype of ClassA.
  ./make_obj_store_test.sh assign_compat_2_${I} unverifiable "$OP" 'class ClassA' 'object'

  # ClassA not interface type.
  #FIXME: this test is valid, you can store type ClassA in a object field
  ./make_obj_store_test.sh assign_compat_3_${I} valid "$OP" object 'class ClassA'
  
  # Implementation of InterfaceB does not require the implementation of InterfaceA
  ./make_obj_store_test.sh assign_compat_4_${I} unverifiable "$OP" 'class InterfaceA' 'class InterfaceB'

  # Array/vector.
  ./make_obj_store_test.sh assign_compat_5_${I} unverifiable "$OP" 'string []' 'string[,]'

  # Vector/array.
  ./make_obj_store_test.sh assign_compat_6_${I} unverifiable "$OP" 'string [,]' 'string[]'

  # Arrays with different rank.
  ./make_obj_store_test.sh assign_compat_7_${I} unverifiable "$OP" 'string [,]' 'string[,,]'

  # Method pointers with different return types.
  ./make_obj_store_test.sh assign_compat_8_${I} unverifiable "$OP" 'method int32 *(int32)' 'method float32 *(int32)'

  # Method pointers with different parameters.
  ./make_obj_store_test.sh assign_compat_9_${I} unverifiable "$OP" 'method int32 *(float64)' 'method int32 *(int32)'

  # Method pointers with different calling conventions.
  ./make_obj_store_test.sh assign_compat_10_${I} unverifiable "$OP" 'method vararg int32 *(int32)' 'method int32 *(int32)'
  
    # Method pointers with different calling conventions. (2)
  ./make_obj_store_test.sh assign_compat_11_${I} unverifiable "$OP" 'method unmanaged fastcall int32 *(int32)' 'method int32 *(int32)'
  
    # Method pointers with different calling conventions. (3)
  ./make_obj_store_test.sh assign_compat_12_${I} unverifiable "$OP" 'method unmanaged fastcall int32 *(int32)' 'method unmanaged stdcall int32 *(int32)'
  
  I=`expr $I + 1`
done

# 1.8.1.3 Merging stack states
I=1
for TYPE1 in int32 int64 'native int' float64 'valuetype ValueType' 'class Class' 'int8&' 'int16&' 'int32&' 'int64&' 'native int&' 'float32&' 'float64&' 'valuetype ValueType&' 'class Class&' 'method int32 *(int32)' 'method float32 *(int32)' 'method int32 *(float64)' 'method vararg int32 *(int32)'
do
  for TYPE2 in int32 int64 'native int' float64 'valuetype ValueType' 'class Class' 'int8&' 'int16&' 'int32&' 'int64&' 'native int&' 'float32&' 'float64&' 'valuetype ValueType&' 'class Class&' 'method int32 *(int32)' 'method float32 *(int32)' 'method int32 *(float64)' 'method vararg int32 *(int32)'
  do
  	ZZ=`echo $TYPE1 | grep "*";`
 	T1_PTR=$?
 	ZZ=`echo $TYPE2 | grep "*";`
 	T2_PTR=$?
 	
    if (($T1_PTR == 0  ||  $T2_PTR == 0)); then
		./make_stack_merge_test.sh stack_merge_${I} unverifiable "$TYPE1" "$TYPE2"
    elif [ "$TYPE1" == "$TYPE2" ]; then
		./make_stack_merge_test.sh stack_merge_${I} valid "$TYPE1" "$TYPE2"
	elif [ "$TYPE1" == "int32" ] && [ "$TYPE2" == "native int" ]; then
		./make_stack_merge_test.sh stack_merge_${I} valid "$TYPE1" "$TYPE2"
	elif [ "$TYPE1" == "native int" ] && [ "$TYPE2" == "int32" ]; then
		./make_stack_merge_test.sh stack_merge_${I} valid "$TYPE1" "$TYPE2"
	elif [ "$TYPE1" == "int32&" ] && [ "$TYPE2" == "native int&" ]; then
		./make_stack_merge_test.sh stack_merge_${I} valid "$TYPE1" "$TYPE2"
	elif [ "$TYPE1" == "native int&" ] && [ "$TYPE2" == "int32&" ]; then
		./make_stack_merge_test.sh stack_merge_${I} valid "$TYPE1" "$TYPE2"
	else
		./make_stack_merge_test.sh stack_merge_${I} unverifiable "$TYPE1" "$TYPE2"
    fi
	I=`expr $I + 1`
  done
done

# Unverifiable array stack merges

# These are verifiable, the merged type is 'object' or 'Array'
#for TYPE1 in 'string []' 'string [,]' 'string [,,]' 
#do
#  for TYPE2 in 'string []' 'string [,]' 'string [,,]' 
#  do
#    if [ "$TYPE1" != "$TYPE2" ]; then
#	./make_stack_merge_test.sh stack_merge_${I} unverifiable "$TYPE1" "$TYPE2"
#	I=`expr $I + 1`
#    fi
#  done
#done

# Exception block branch tests (see 3.15)
I=1
for OP in br "ldc.i4.0\n\tbrfalse"
do
  ./make_exception_branch_test.sh in_try_${I} "$OP branch_target1"
  ./make_exception_branch_test.sh in_catch_${I} "$OP branch_target2"
  ./make_exception_branch_test.sh in_finally_${I} "$OP branch_target3"
  ./make_exception_branch_test.sh in_filter_${I} "$OP branch_target4"
  ./make_exception_branch_test.sh out_try_${I} "" "$OP branch_target5"
  ./make_exception_branch_test.sh out_catch_${I} "" "" "$OP branch_target5"
  ./make_exception_branch_test.sh out_finally_${I} "" "" "" "$OP branch_target5"
  ./make_exception_branch_test.sh out_filter_${I} "" "" "" "" "$OP branch_target5"
  I=`expr $I + 1`
done

./make_exception_branch_test.sh ret_out_try "" "ldc.i4.0\n\tret"
./make_exception_branch_test.sh ret_out_catch "" "" "ldc.i4.0\n\tret"
./make_exception_branch_test.sh ret_out_finally "" "" "" "ldc.i4.0\n\tret"
./make_exception_branch_test.sh ret_out_filter "" "" "" "" "ldc.i4.0\n\tret"

# Unary branch op type tests (see 3.17)

for OP in brfalse
do
  ./make_unary_test.sh un_branch_op unverifiable "$OP branch_target" float64
done

# Ldloc.0 and Ldarg tests (see 3.38)

I=1
for OP in "ldarg.s 0" "ldarg.0"
do
  ./make_unary_test.sh ld_no_slot_${I} unverifiable "pop\n\t$OP\n\tpop" int32
  I=`expr $I + 1`
done

for OP in "ldloc.s 1" "ldloc.1" "ldloc 1"
do
  ./make_unary_test.sh ld_no_slot_${I} invalid "pop\n\t$OP\n\tpop" int32
  I=`expr $I + 1`
done

for OP in "ldarga.s 0" "ldloca.s 1"
do
  ./make_unary_test.sh ld_no_slot_${I} invalid "pop\n\t$OP\n\tpop" int32
  I=`expr $I + 1`
done

# Starg and Stloc tests (see 3.61)

I=1
for OP in "starg.s 0"
do
  ./make_unary_test.sh st_no_slot_${I} unverifiable "$OP" int32
  I=`expr $I + 1`
done

for OP in "stloc.s 1"
do
  ./make_unary_test.sh st_no_slot_${I} invalid "$OP" int32
  I=`expr $I + 1`
done

# Ldfld and Ldflda tests (see 4.10)

for OP in ldfld ldflda
do
  ./make_unary_test.sh ${OP}_no_fld invalid "$OP int32 Class::invalid\n\tpop" "class Class"
  ./make_unary_test.sh ${OP}_bad_obj unverifiable "$OP int32 Class::valid\n\tpop" object
  ./make_unary_test.sh ${OP}_obj_int32 unverifiable "$OP int32 Class::valid\n\tpop" int32
  ./make_unary_test.sh ${OP}_obj_int64 unverifiable "$OP int32 Class::valid\n\tpop" int64
  ./make_unary_test.sh ${OP}_obj_float64 unverifiable "$OP int32 Class::valid\n\tpop" float64
  ./make_unary_test.sh ${OP}_obj_native_int unverifiable "$OP int32 Class::valid\n\tpop" 'native int'
  ./make_unary_test.sh ${OP}_obj_ref_overlapped unverifiable "$OP object Overlapped::objVal\n\tpop" "class Overlapped"
  ./make_unary_test.sh ${OP}_obj_overlapped_field_not_accessible unverifiable "$OP int32 Overlapped::publicIntVal\n\tpop" "class Overlapped"
done

#TODO: these tests are bogus, they need to be fixed
# Stfld tests (see 4.28)

./make_unary_test.sh stfld_no_fld invalid "ldc.i4.0\n\tstfld int32 Class::invalid" "class Class"
./make_unary_test.sh stfld_bad_obj unverifiable "ldc.i4.0\n\tstfld int32 Class::valid" object
./make_unary_test.sh stfld_obj_int32 unverifiable "ldc.i4.0\n\tstfld int32 Class::valid" int32
./make_unary_test.sh stfld_obj_int64 unverifiable "ldc.i4.0\n\tstfld int32 Class::valid" int64
./make_unary_test.sh stfld_obj_float64 unverifiable "ldc.i4.0\n\tstfld int32 Class::valid" float64
./make_unary_test.sh stfld_no_int invalid "stfld int32 Class::valid" "class Class"
./make_unary_test.sh stfld_obj_native_int unverifiable "ldc.i4.0\n\tstfld int32 Class::valid" 'native int'

# Box tests (see 4.1)

# Box non-existent type.
./make_unary_test.sh box_bad_type unverifiable "box valuetype NonExistent\n\tpop" "valuetype NonExistent"

# Top of stack not assignment compatible with typeToc.
./make_unary_test.sh box_not_compat unverifiable "box [mscorlib]System.Int32\n\tpop" float32

# Box byref type.
./make_unary_test.sh box_byref invalid "box [mscorlib]System.Int32\&\n\tpop" 'int32&'

# Box byref-like type.
./make_unary_test.sh box_byref_like unverifiable "box [mscorlib]System.TypedReference\n\tpop" typedref

#This is illegal since you cannot have a Void local variable, it should go into the structural tests part
# Box void type.
#./make_unary_test.sh box_void unverifiable "box [mscorlib]System.Void\n\tpop" "class [mscorlib]System.Void"





./make_ret_test.sh ret_coercion_1 unverifiable int8 int64
./make_ret_test.sh ret_coercion_2 unverifiable int8 float64
./make_ret_test.sh ret_coercion_3 unverifiable int8 'int8&'
./make_ret_test.sh ret_coercion_4 unverifiable int8 object

./make_ret_test.sh ret_coercion_5 unverifiable 'unsigned int8' int64
./make_ret_test.sh ret_coercion_6 unverifiable 'unsigned int8' float64
./make_ret_test.sh ret_coercion_6 unverifiable 'unsigned int8' float64
./make_ret_test.sh ret_coercion_6 unverifiable 'unsigned int8' float64
./make_ret_test.sh ret_coercion_7 unverifiable 'unsigned int8' 'unsigned int8&'
./make_ret_test.sh ret_coercion_8 unverifiable 'unsigned int8' object

./make_ret_test.sh ret_coercion_9 unverifiable bool int64
./make_ret_test.sh ret_coercion_10 unverifiable bool float64
./make_ret_test.sh ret_coercion_11 unverifiable bool 'bool&'
./make_ret_test.sh ret_coercion_12 unverifiable bool object

./make_ret_test.sh ret_coercion_13 unverifiable int16 int64
./make_ret_test.sh ret_coercion_14 unverifiable int16 float64
./make_ret_test.sh ret_coercion_15 unverifiable int16 'int16&'
./make_ret_test.sh ret_coercion_16 unverifiable int16 object
  
./make_ret_test.sh ret_coercion_17 unverifiable 'unsigned int16' int64
./make_ret_test.sh ret_coercion_18 unverifiable 'unsigned int16' float64
./make_ret_test.sh ret_coercion_19 unverifiable 'unsigned int16' 'unsigned int16&'
./make_ret_test.sh ret_coercion_20 unverifiable 'unsigned int16' object
  
./make_ret_test.sh ret_coercion_21 unverifiable char int64
./make_ret_test.sh ret_coercion_22 unverifiable char float64
./make_ret_test.sh ret_coercion_23 unverifiable char 'char&'
./make_ret_test.sh ret_coercion_24 unverifiable char object
  
./make_ret_test.sh ret_coercion_25 unverifiable int32 int64
./make_ret_test.sh ret_coercion_26 unverifiable int32 float64
./make_ret_test.sh ret_coercion_27 unverifiable int32 'int32&'
./make_ret_test.sh ret_coercion_28 unverifiable int32 object
  
./make_ret_test.sh ret_coercion_29 unverifiable 'unsigned int32' int64
./make_ret_test.sh ret_coercion_30 unverifiable 'unsigned int32' float64
./make_ret_test.sh ret_coercion_31 unverifiable 'unsigned int32' 'unsigned int32&'
./make_ret_test.sh ret_coercion_32 unverifiable 'unsigned int32' object
 
./make_ret_test.sh ret_coercion_33 unverifiable int64 int32
./make_ret_test.sh ret_coercion_34 unverifiable int64 'native int'
./make_ret_test.sh ret_coercion_35 unverifiable int64 float64
./make_ret_test.sh ret_coercion_36 unverifiable int64 'int64&'
./make_ret_test.sh ret_coercion_37 unverifiable int64 object
  
./make_ret_test.sh ret_coercion_38 unverifiable 'unsigned int64' int32
./make_ret_test.sh ret_coercion_39 unverifiable 'unsigned int64' 'native int'
./make_ret_test.sh ret_coercion_40 unverifiable 'unsigned int64' float64
./make_ret_test.sh ret_coercion_41 unverifiable 'unsigned int64' 'unsigned int64&'
./make_ret_test.sh ret_coercion_42 unverifiable 'unsigned int64' object
  
./make_ret_test.sh ret_coercion_43 unverifiable 'native int' int64
./make_ret_test.sh ret_coercion_44 unverifiable 'native int' float64
./make_ret_test.sh ret_coercion_45 unverifiable 'native int' 'native int&'
./make_ret_test.sh ret_coercion_46 unverifiable 'native int' object
  
./make_ret_test.sh ret_coercion_47 unverifiable 'native unsigned int' int64
./make_ret_test.sh ret_coercion_48 unverifiable 'native unsigned int' float64
./make_ret_test.sh ret_coercion_49 unverifiable 'native unsigned int' 'native unsigned int&'
./make_ret_test.sh ret_coercion_50 unverifiable 'native unsigned int' object
  
./make_ret_test.sh ret_coercion_51 unverifiable float32 int32
./make_ret_test.sh ret_coercion_52 unverifiable float32 'native int'
./make_ret_test.sh ret_coercion_53 unverifiable float32 int64
./make_ret_test.sh ret_coercion_54 unverifiable float32 'float32&'
./make_ret_test.sh ret_coercion_55 unverifiable float32 object
  
./make_ret_test.sh ret_coercion_56 unverifiable float64 int32
./make_ret_test.sh ret_coercion_57 unverifiable float64 'native int'
./make_ret_test.sh ret_coercion_58 unverifiable float64 int64
./make_ret_test.sh ret_coercion_59 unverifiable float64 'float64&'
./make_ret_test.sh ret_coercion_60 unverifiable float64 object

./make_ret_test.sh ret_coercion_61 unverifiable object int32
./make_ret_test.sh ret_coercion_62 unverifiable object 'native int'
./make_ret_test.sh ret_coercion_63 unverifiable object int64
./make_ret_test.sh ret_coercion_64 unverifiable object float64
./make_ret_test.sh ret_coercion_65 unverifiable object 'object&'
  
./make_ret_test.sh ret_coercion_66 unverifiable 'class MyValueType' int32
./make_ret_test.sh ret_coercion_67 unverifiable 'class MyValueType' 'native int'
./make_ret_test.sh ret_coercion_68 unverifiable 'class MyValueType' int64
./make_ret_test.sh ret_coercion_69 unverifiable 'class MyValueType' float64
./make_ret_test.sh ret_coercion_70 unverifiable 'class MyValueType' 'class MyValueType&'
./make_ret_test.sh ret_coercion_71 unverifiable 'class MyValueType' object
  
./make_ret_test.sh ret_coercion_72 unverifiable 'int32&' int32
./make_ret_test.sh ret_coercion_73 unverifiable 'int32&' 'native int'
./make_ret_test.sh ret_coercion_74 unverifiable 'int32&' int64
./make_ret_test.sh ret_coercion_75 unverifiable 'int32&' float64
./make_ret_test.sh ret_coercion_76 unverifiable 'int32&' object
  
./make_ret_test.sh ret_coercion_77 unverifiable typedref int32
./make_ret_test.sh ret_coercion_78 unverifiable typedref 'native int'
./make_ret_test.sh ret_coercion_79 unverifiable typedref int64
./make_ret_test.sh ret_coercion_80 unverifiable typedref float64
./make_ret_test.sh ret_coercion_81 unverifiable typedref 'typedref&'
./make_ret_test.sh ret_coercion_82 unverifiable typedref object


./make_ret_test.sh ret_sub_type valid ClassA ClassSubA
./make_ret_test.sh ret_same_type valid ClassA ClassA
./make_ret_test.sh ret_obj_iface valid object InterfaceA
./make_ret_test.sh ret_obj_obj valid object object
./make_ret_test.sh ret_obj_string valid object string
./make_ret_test.sh ret_string_string valid string string
./make_ret_test.sh ret_obj_vector valid object 'int32[]'
./make_ret_test.sh ret_obj_array valid object 'int32[,]'
./make_ret_test.sh ret_obj_generic valid object 'class Template`1<object>'
./make_ret_test.sh ret_obj_value_type unverifiable object 'MyValueType'
./make_ret_test.sh ret_string_value_type unverifiable string 'MyValueType'
./make_ret_test.sh ret_class_value_type unverifiable ClassA 'MyValueType'

./make_ret_test.sh ret_string_string unverifiable string object
./make_ret_test.sh ret_string_string unverifiable 'int32[]' object

./make_ret_test.sh ret_iface_imple valid InterfaceA ImplA
./make_ret_test.sh ret_arrays_same_vector valid 'int32[]' 'int32[]'
./make_ret_test.sh ret_arrays_same_rank valid 'int32[,]' 'int32[,]'

./make_ret_test.sh ret_sub_type_array_covariant valid 'ClassA[]' 'ClassSubA[]'
./make_ret_test.sh ret_same_type_array_covariant valid 'ClassA[]' 'ClassA[]'
./make_ret_test.sh ret_obj_iface_array_covariant valid 'object[]' 'InterfaceA[]'
./make_ret_test.sh ret_iface_imple_array_covariant valid 'InterfaceA[]' 'ImplA[]'

./make_ret_test.sh ret_diff_types unverifiable ClassA ClassB
./make_ret_test.sh ret_class_vale_type unverifiable ClassA MyValueType
./make_ret_test.sh ret_diff_vale_type unverifiable MyValueType2 MyValueType
./make_ret_test.sh ret_value_type_class unverifiable MyValueType ClassA
./make_ret_test.sh ret_super_type unverifiable ClassSubA ClassB
./make_ret_test.sh ret_interfaces unverifiable InterfaceA InterfaceB
./make_ret_test.sh ret_interface_class unverifiable ClassA InterfaceB

./make_ret_test.sh ret_object_type valid object ClassA
./make_ret_test.sh ret_type_object unverifiable ClassA object


./make_ret_test.sh ret_array_diff_rank unverifiable 'int32[]' 'int32[,]'
./make_ret_test.sh ret_array_diff_rank2 unverifiable 'int32[,]' 'int32[]'
./make_ret_test.sh ret_array_diff_rank3 unverifiable 'int32[,,]' 'int32[,]'
./make_ret_test.sh ret_array_not_covar unverifiable 'ClassA[]' 'ClassB[]'
./make_ret_test.sh ret_array_not_covar2 unverifiable 'ClassSubA[]' 'ClassA[]'
./make_ret_test.sh ret_array_not_covar3 unverifiable 'ClassA[]' 'InterfaceA[]'
./make_ret_test.sh ret_array_not_covar4 unverifiable 'ImplA[]' 'InterfaceA[]'
./make_ret_test.sh ret_array_not_covar5 unverifiable 'InterfaceA[]' 'object[]'


#generics tests
./make_ret_test.sh ret_generics_1 valid 'class Template' 'class Template'
./make_ret_test.sh ret_generics_2 valid 'class Template`1<int32>' 'class Template`1<int32>'
./make_ret_test.sh ret_generics_3 valid 'class Template`2<int32,object>' 'class Template`2<int32,object>'

./make_ret_test.sh ret_generics_4 unverifiable 'class Template' 'class Template`1<object>'
./make_ret_test.sh ret_generics_5 unverifiable 'class Template`1<object>' 'class Template'
./make_ret_test.sh ret_generics_6 unverifiable 'class Template`1<object>' 'class Template`1<string>'
./make_ret_test.sh ret_generics_7 unverifiable 'class Template`1<string>' 'class Template`1<object>'
./make_ret_test.sh ret_generics_8 unverifiable 'class Template`1<object>' 'class Template`2<object, object>'
./make_ret_test.sh ret_generics_9 unverifiable 'class Template`2<object, object>' 'class Template`1<object>'

./make_ret_test.sh ret_generics_10 unverifiable 'class Template`1<int32>' 'class Template`1<int16>'
./make_ret_test.sh ret_generics_11 unverifiable 'class Template`1<int16>' 'class Template`1<int32>'
./make_ret_test.sh ret_generics_12 unverifiable 'class Template`1<unsigned int32>' 'class Template`1<int32>'
./make_ret_test.sh ret_generics_13 unverifiable 'class Template`1<float32>' 'class Template`1<float64>'
./make_ret_test.sh ret_generics_14 unverifiable 'class Template`1<float64>' 'class Template`1<float32>'

#variance tests
./make_ret_test.sh ret_generics_15 valid 'class ICovariant`1<object>' 'class ICovariant`1<string>'
./make_ret_test.sh ret_generics_16 valid 'class ICovariant`1<string>' 'class ICovariant`1<string>'
./make_ret_test.sh ret_generics_17 unverifiable 'class ICovariant`1<string>' 'class ICovariant`1<object>'

./make_ret_test.sh ret_generics_18 valid 'class IContravariant`1<string>' 'class IContravariant`1<object>'
./make_ret_test.sh ret_generics_19 valid 'class IContravariant`1<string>' 'class IContravariant`1<string>'
./make_ret_test.sh ret_generics_20 unverifiable 'class IContravariant`1<object>' 'class IContravariant`1<string>'

./make_ret_test.sh ret_generics_21 valid 'class ICovariant`1<ClassA>' 'class ICovariant`1<ClassSubA>'
./make_ret_test.sh ret_generics_22 valid 'class ICovariant`1<ClassSubA>' 'class ICovariant`1<ClassSubA>'
./make_ret_test.sh ret_generics_23 unverifiable 'class ICovariant`1<ClassSubA>' 'class ICovariant`1<ClassA>'

./make_ret_test.sh ret_generics_24 valid 'class IContravariant`1<ClassSubA>' 'class IContravariant`1<ClassA>'
./make_ret_test.sh ret_generics_25 valid 'class IContravariant`1<ClassSubA>' 'class IContravariant`1<ClassSubA>'
./make_ret_test.sh ret_generics_26 unverifiable 'class IContravariant`1<ClassA>' 'class IContravariant`1<ClassSubA>'


./make_ret_test.sh ret_generics_27 valid 'class Bivariant`2<ClassA, ClassB>' 'class Bivariant`2<ClassA, ClassB>'
./make_ret_test.sh ret_generics_28 valid 'class Bivariant`2<ClassA, ClassB>' 'class Bivariant`2<ClassA, object>'
./make_ret_test.sh ret_generics_29 valid 'class Bivariant`2<ClassA, ClassB>' 'class Bivariant`2<ClassSubA, ClassB>'
./make_ret_test.sh ret_generics_30 valid 'class Bivariant`2<ClassA, ClassB>' 'class Bivariant`2<ClassSubA, object>'
./make_ret_test.sh ret_generics_31 unverifiable 'class Bivariant`2<ClassA, ClassB>' 'class Bivariant`2<object, ClassB>'
./make_ret_test.sh ret_generics_32 unverifiable 'class Bivariant`2<ClassA, ClassB>' 'class Bivariant`2<object, object>'
./make_ret_test.sh ret_generics_33 unverifiable 'class Bivariant`2<ClassA, object>' 'class Bivariant`2<object, ClassB>'
./make_ret_test.sh ret_generics_34 unverifiable 'class Bivariant`2<ClassA, object>' 'class Bivariant`2<ClassA, ClassB>'

#mix parameter types
./make_ret_test.sh ret_generics_types_1 unverifiable 'class Template`1<int8>' 'class Template`1<unsigned int8>'
./make_ret_test.sh ret_generics_types_2 unverifiable 'class Template`1<int8>' 'class Template`1<int16>'
./make_ret_test.sh ret_generics_types_3 unverifiable 'class Template`1<int8>' 'class Template`1<unsigned int16>'
./make_ret_test.sh ret_generics_types_4 unverifiable 'class Template`1<int8>' 'class Template`1<int32>'
./make_ret_test.sh ret_generics_types_5 unverifiable 'class Template`1<int8>' 'class Template`1<unsigned int32>'
./make_ret_test.sh ret_generics_types_6 unverifiable 'class Template`1<int8>' 'class Template`1<int64>'
./make_ret_test.sh ret_generics_types_7 unverifiable 'class Template`1<int8>' 'class Template`1<unsigned int64>'
./make_ret_test.sh ret_generics_types_8 unverifiable 'class Template`1<int8>' 'class Template`1<float32>'
./make_ret_test.sh ret_generics_types_9 unverifiable 'class Template`1<int8>' 'class Template`1<float64>'
./make_ret_test.sh ret_generics_types_10 unverifiable 'class Template`1<int8>' 'class Template`1<bool>'

./make_ret_test.sh ret_generics_types_11 unverifiable 'class Template`1<int8>' 'class Template`1<native int>'
./make_ret_test.sh ret_generics_types_12 unverifiable 'class Template`1<int8>' 'class Template`1<native unsigned int>'
./make_ret_test.sh ret_generics_types_13 unverifiable 'class Template`1<int8>' 'class Template`1<int32 *>'


#inheritance tests
./make_ret_test.sh ret_generics_inheritante_1 valid 'class Base`1<int32>' 'class SubClass1`1<int32>'
./make_ret_test.sh ret_generics_inheritante_2 valid 'class SubClass1`1<int32>' 'class SubClass1`1<int32>'
./make_ret_test.sh ret_generics_inheritante_3 unverifiable 'class SubClass1`1<int32>' 'class Base`1<int32>'
./make_ret_test.sh ret_generics_inheritante_4 unverifiable 'class Base`1<int32>' 'class SubClass1`1<float32>'
./make_ret_test.sh ret_generics_inheritante_5 valid 'class Base`1<object>' 'class SubClass1`1<object>'

./make_ret_test.sh ret_generics_inheritante_6 valid 'class BaseBase`2<int32, object>' 'class SubClass1`1<object>'
./make_ret_test.sh ret_generics_inheritante_7 valid 'class BaseBase`2<int32, object>' 'class Base`1<object>'

./make_ret_test.sh ret_generics_inheritante_8 unverifiable 'class BaseBase`2<int64, object>' 'class Base`1<object>'
./make_ret_test.sh ret_generics_inheritante_9 unverifiable 'class BaseBase`2<int64, object>' 'class SubClass1`1<object>'
./make_ret_test.sh ret_generics_inheritante_10 unverifiable 'class BaseBase`2<int32, object>' 'class SubClass1`1<string>'

#interface tests

./make_ret_test.sh ret_generics_inheritante_12 valid 'class Interface`1<int32>' 'class InterfaceImpl`1<int32>'
./make_ret_test.sh ret_generics_inheritante_13 valid 'class InterfaceImpl`1<int32>' 'class InterfaceImpl`1<int32>'
./make_ret_test.sh ret_generics_inheritante_14 unverifiable 'class InterfaceImpl`1<int32>' 'class Interface`1<int32>'
./make_ret_test.sh ret_generics_inheritante_15 unverifiable 'class Interface`1<int32>' 'class InterfaceImpl`1<float32>'
./make_ret_test.sh ret_generics_inheritante_16 valid 'class Interface`1<object>' 'class InterfaceImpl`1<object>'


#mix variance with inheritance
#only interfaces or delegates can have covariance

#mix variance with interfaces

./make_ret_test.sh ret_generics_inheritante_28 valid 'class ICovariant`1<object>' 'class CovariantImpl`1<string>'
./make_ret_test.sh ret_generics_inheritante_29 valid 'class ICovariant`1<string>' 'class CovariantImpl`1<string>'
./make_ret_test.sh ret_generics_inheritante_30 unverifiable 'class ICovariant`1<string>' 'class CovariantImpl`1<object>'

./make_ret_test.sh ret_generics_inheritante_31 valid 'class IContravariant`1<string>' 'class ContravariantImpl`1<object>'
./make_ret_test.sh ret_generics_inheritante_32 valid 'class IContravariant`1<string>' 'class ContravariantImpl`1<string>'
./make_ret_test.sh ret_generics_inheritante_33 unverifiable 'class IContravariant`1<object>' 'class ContravariantImpl`1<string>'

./make_ret_test.sh ret_generics_inheritante_34 valid 'class ICovariant`1<ClassA>' 'class CovariantImpl`1<ClassSubA>'
./make_ret_test.sh ret_generics_inheritante_35 valid 'class ICovariant`1<ClassSubA>' 'class CovariantImpl`1<ClassSubA>'
./make_ret_test.sh ret_generics_inheritante_36 unverifiable 'class ICovariant`1<ClassSubA>' 'class CovariantImpl`1<ClassA>'

./make_ret_test.sh ret_generics_inheritante_37 valid 'class IContravariant`1<ClassSubA>' 'class ContravariantImpl`1<ClassA>'
./make_ret_test.sh ret_generics_inheritante_38 valid 'class IContravariant`1<ClassSubA>' 'class ContravariantImpl`1<ClassSubA>'
./make_ret_test.sh ret_generics_inheritante_39 unverifiable 'class IContravariant`1<ClassA>' 'class ContravariantImpl`1<ClassSubA>'


#mix variance with arrays

./make_ret_test.sh ret_generics_arrays_1 valid 'class ICovariant`1<object>' 'class ICovariant`1<object[]>'
./make_ret_test.sh ret_generics_arrays_2 valid 'class ICovariant`1<object>' 'class ICovariant`1<int32[]>'
./make_ret_test.sh ret_generics_arrays_3 valid 'class ICovariant`1<object>' 'class ICovariant`1<int32[,]>'
./make_ret_test.sh ret_generics_arrays_4 valid 'class ICovariant`1<object>' 'class ICovariant`1<string[]>'
./make_ret_test.sh ret_generics_arrays_5 valid 'class ICovariant`1<object[]>' 'class ICovariant`1<string[]>'
./make_ret_test.sh ret_generics_arrays_6 valid 'class ICovariant`1<object[]>' 'class ICovariant`1<ClassA[]>'
./make_ret_test.sh ret_generics_arrays_7 valid 'class ICovariant`1<ClassA[]>' 'class ICovariant`1<ClassSubA[]>'
./make_ret_test.sh ret_generics_arrays_8 valid 'class ICovariant`1<InterfaceA[]>' 'class ICovariant`1<ImplA[]>'
./make_ret_test.sh ret_generics_arrays_9 valid 'class ICovariant`1<object[,]>' 'class ICovariant`1<string[,]>'
./make_ret_test.sh ret_generics_arrays_10 valid 'class ICovariant`1<ClassA[,]>' 'class ICovariant`1<ClassSubA[,]>'

./make_ret_test.sh ret_generics_arrays_1_b valid 'class ICovariant`1<object>' 'class CovariantImpl`1<object[]>'
./make_ret_test.sh ret_generics_arrays_2_b valid 'class ICovariant`1<object>' 'class CovariantImpl`1<int32[]>'
./make_ret_test.sh ret_generics_arrays_3_b valid 'class ICovariant`1<object>' 'class CovariantImpl`1<int32[,]>'
./make_ret_test.sh ret_generics_arrays_4_b valid 'class ICovariant`1<object>' 'class ICovariant`1<string[]>'
./make_ret_test.sh ret_generics_arrays_5_b valid 'class ICovariant`1<object[]>' 'class CovariantImpl`1<string[]>'
./make_ret_test.sh ret_generics_arrays_6_b valid 'class ICovariant`1<object[]>' 'class CovariantImpl`1<ClassA[]>'
./make_ret_test.sh ret_generics_arrays_7_b valid 'class ICovariant`1<ClassA[]>' 'class CovariantImpl`1<ClassSubA[]>'
./make_ret_test.sh ret_generics_arrays_8_b valid 'class ICovariant`1<InterfaceA[]>' 'class CovariantImpl`1<ImplA[]>'
./make_ret_test.sh ret_generics_arrays_9_b valid 'class ICovariant`1<object[,]>' 'class CovariantImpl`1<string[,]>'
./make_ret_test.sh ret_generics_arrays_10_b valid 'class ICovariant`1<ClassA[,]>' 'class CovariantImpl`1<ClassSubA[,]>'

./make_ret_test.sh ret_generics_arrays_11 valid 'class IContravariant`1<object[]>' 'class IContravariant`1<object>'
./make_ret_test.sh ret_generics_arrays_12 valid 'class IContravariant`1<int32[]>' 'class IContravariant`1<object>'
./make_ret_test.sh ret_generics_arrays_13 valid 'class IContravariant`1<int32[,]>' 'class IContravariant`1<object>'
./make_ret_test.sh ret_generics_arrays_14 valid 'class IContravariant`1<string[]>' 'class IContravariant`1<object>'
./make_ret_test.sh ret_generics_arrays_15 valid 'class IContravariant`1<string[]>' 'class IContravariant`1<object[]>'
./make_ret_test.sh ret_generics_arrays_16 valid 'class IContravariant`1<ClassA[]>' 'class IContravariant`1<object[]>'
./make_ret_test.sh ret_generics_arrays_17 valid 'class IContravariant`1<ClassSubA[]>' 'class IContravariant`1<ClassA[]>'
./make_ret_test.sh ret_generics_arrays_18 valid 'class IContravariant`1<ImplA[]>' 'class IContravariant`1<InterfaceA[]>'
./make_ret_test.sh ret_generics_arrays_19 valid 'class IContravariant`1<string[,]>' 'class IContravariant`1<object[,]>'
./make_ret_test.sh ret_generics_arrays_20 valid 'class IContravariant`1<ClassSubA[,]>' 'class IContravariant`1<ClassA[,]>'

./make_ret_test.sh ret_generics_arrays_11_b valid 'class IContravariant`1<object[]>' 'class ContravariantImpl`1<object>'
./make_ret_test.sh ret_generics_arrays_12_b valid 'class IContravariant`1<int32[]>' 'class ContravariantImpl`1<object>'
./make_ret_test.sh ret_generics_arrays_13_b valid 'class IContravariant`1<int32[,]>' 'class ContravariantImpl`1<object>'
./make_ret_test.sh ret_generics_arrays_14_b valid 'class IContravariant`1<string[]>' 'class ContravariantImpl`1<object>'
./make_ret_test.sh ret_generics_arrays_15_b valid 'class IContravariant`1<string[]>' 'class ContravariantImpl`1<object[]>'
./make_ret_test.sh ret_generics_arrays_16_b valid 'class IContravariant`1<ClassA[]>' 'class ContravariantImpl`1<object[]>'
./make_ret_test.sh ret_generics_arrays_17_b valid 'class IContravariant`1<ClassSubA[]>' 'class ContravariantImpl`1<ClassA[]>'
./make_ret_test.sh ret_generics_arrays_18_b valid 'class IContravariant`1<ImplA[]>' 'class ContravariantImpl`1<InterfaceA[]>'
./make_ret_test.sh ret_generics_arrays_19_b valid 'class IContravariant`1<string[,]>' 'class ContravariantImpl`1<object[,]>'
./make_ret_test.sh ret_generics_arrays_20_b valid 'class IContravariant`1<ClassSubA[,]>' 'class ContravariantImpl`1<ClassA[,]>'

./make_ret_test.sh ret_generics_arrays_21 unverifiable 'class ICovariant`1<int32[]>' 'class ICovariant`1<object>'
./make_ret_test.sh ret_generics_arrays_22 unverifiable 'class ICovariant`1<int32[]>' 'class ICovariant`1<object[]>'
./make_ret_test.sh ret_generics_arrays_23 unverifiable 'class ICovariant`1<string[]>' 'class ICovariant`1<object[]>'
./make_ret_test.sh ret_generics_arrays_24 unverifiable 'class ICovariant`1<ClassSubA[]>' 'class ICovariant`1<ClassA[]>'
./make_ret_test.sh ret_generics_arrays_25 unverifiable 'class ICovariant`1<int32[]>' 'class ICovariant`1<int32[,]>'
./make_ret_test.sh ret_generics_arrays_26 unverifiable 'class ICovariant`1<ImplA[]>' 'class ICovariant`1<InterfaceA[]>'

./make_ret_test.sh ret_generics_arrays_27 unverifiable 'class IContravariant`1<object>' 'class IContravariant`1<int32[]>'
./make_ret_test.sh ret_generics_arrays_28 unverifiable 'class IContravariant`1<object[]>' 'class IContravariant`1<int32[]>'
./make_ret_test.sh ret_generics_arrays_29 unverifiable 'class IContravariant`1<object[]>' 'class IContravariant`1<string[]>'
./make_ret_test.sh ret_generics_arrays_30 unverifiable 'class IContravariant`1<ClassA[]>' 'class IContravariant`1<ClassSubA[]>'
./make_ret_test.sh ret_generics_arrays_31 unverifiable 'class IContravariant`1<int32[,]>' 'class IContravariant`1<int32[]>'
./make_ret_test.sh ret_generics_arrays_32 unverifiable 'class IContravariant`1<InterfaceA[]>' 'class IContravariant`1<ImplA[]>'


#generic with value types

./make_ret_test.sh ret_generics_vt_1 valid 'class Template`1<MyValueType>' 'class Template`1<MyValueType>'
./make_ret_test.sh ret_generics_vt_2 unverifiable 'class Template`1<MyValueType>' 'class Template`1<MyValueType2>'
./make_ret_test.sh ret_generics_vt_3 unverifiable 'class ICovariant`1<MyValueType>' 'class ICovariant`1<MyValueType2>'
./make_ret_test.sh ret_generics_vt_4 unverifiable 'class ICovariant`1<object>' 'class ICovariant`1<MyValueType2>'


#mix variance and generic compatibility with all kinds of types valid for a generic parameter (hellish task - huge task)
#test with composite generics ( Foo<Bar<int>> )

#test variance with delegates
#generic methods
#generic atributes
#generic delegates
#generic code
#the verifier must check if the generic instantiation is valid

for OP in ldarg ldloc
do
	ARGS_1='int32 V'
	LOCALS_1=''
	CALL_1='ldc.i4.0'
	SIG_1='int32'
	
	ARGS_2='int32 V, int32 V1'
	LOCALS_2=''
	CALL_2='ldc.i4.0\n\tldc.i4.0'
	SIG_2='int32, int32'
	
	ARGS_3='int32 V, int32 V1, int32 V1'
	LOCALS_3=''
	CALL_3='ldc.i4.0\n\tldc.i4.0\n\tldc.i4.0'
	SIG_3='int32, int32, int32'
	
	ARGS_4='int32 V, int32 V1, int32 V1, int32 V1'
	LOCALS_4=''
	CALL_4='ldc.i4.0\n\tldc.i4.0\n\tldc.i4.0\n\tldc.i4.0'
	SIG_4='int32, int32, int32, int32'
	MAX_PARAM_RESULT="unverifiable"
	POPS="pop\npop\npop\npop\npop\npop\npop\npop\n"
	
	if [ "$OP" == "ldloc" ]; then
		MAX_PARAM_RESULT="invalid"

		LOCALS_1=$ARGS_1
		ARGS_1=''
		CALL_1=''
		SIG_1=''

		LOCALS_2=$ARGS_2
		ARGS_2=''
		CALL_2=''
		SIG_2=''

		LOCALS_3=$ARGS_3
		ARGS_3=''
		CALL_3=''
		SIG_3=''

		LOCALS_4=$ARGS_4
		ARGS_4=''
		CALL_4=''
		SIG_4=''
	fi;
	
	./make_load_test.sh ${OP}0_max_params "${MAX_PARAM_RESULT}" "${OP}.0" '' '' '' ''
	./make_load_test.sh ${OP}1_max_params "${MAX_PARAM_RESULT}" "${OP}.1" '' '' '' ''
	./make_load_test.sh ${OP}2_max_params "${MAX_PARAM_RESULT}" "${OP}.2" '' '' '' ''
	./make_load_test.sh ${OP}3_max_params "${MAX_PARAM_RESULT}" "${OP}.3" '' '' '' ''
	
	./make_load_test.sh ${OP}1_1_max_params "${MAX_PARAM_RESULT}" "${OP}.1" "${ARGS_1}" "${LOCALS_1}" "${CALL_1}" "${SIG_1}"
	./make_load_test.sh ${OP}2_1_max_params "${MAX_PARAM_RESULT}" "${OP}.2" "${ARGS_1}" "${LOCALS_1}" "${CALL_1}" "${SIG_1}"
	./make_load_test.sh ${OP}3_1_max_params "${MAX_PARAM_RESULT}" "${OP}.3" "${ARGS_1}" "${LOCALS_1}" "${CALL_1}" "${SIG_1}"
	
	./make_load_test.sh ${OP}2_2_max_params "${MAX_PARAM_RESULT}" "${OP}.2" "${ARGS_2}" "${LOCALS_2}" "${CALL_2}" "${SIG_2}"
	./make_load_test.sh ${OP}3_2_max_params "${MAX_PARAM_RESULT}" "${OP}.3" "${ARGS_2}" "${LOCALS_2}" "${CALL_2}" "${SIG_2}"
	
	./make_load_test.sh ${OP}3_3_max_params "${MAX_PARAM_RESULT}" "${OP}.3" "${ARGS_3}" "${LOCALS_3}" "${CALL_3}" "${SIG_3}"
	
	./make_load_test.sh ${OP}0_max_params valid "${OP}.0" "${ARGS_1}" "${LOCALS_1}" "${CALL_1}" "${SIG_1}"
	./make_load_test.sh ${OP}1_max_params valid "${OP}.1" "${ARGS_2}" "${LOCALS_2}" "${CALL_2}" "${SIG_2}"
	./make_load_test.sh ${OP}2_max_params valid "${OP}.2" "${ARGS_3}" "${LOCALS_3}" "${CALL_3}" "${SIG_3}"
	./make_load_test.sh ${OP}3_max_params valid "${OP}.3" "${ARGS_4}" "${LOCALS_4}" "${CALL_4}" "${SIG_4}"
	
	./make_load_test.sh ${OP}0_stack_overflow invalid "${OP}.0\n${OP}.0\n${OP}.0\n${OP}.0\n${OP}.0\n${OP}.0\n${OP}.0\n${OP}.0\n${OP}.0\n${POPS}" "${ARGS_4}" "${LOCALS_4}" "${CALL_4}" "${SIG_4}"
	./make_load_test.sh ${OP}1_stack_overflow invalid "${OP}.1\n${OP}.1\n${OP}.1\n${OP}.1\n${OP}.1\n${OP}.1\n${OP}.1\n${OP}.1\n${OP}.1\n${POPS}" "${ARGS_4}" "${LOCALS_4}" "${CALL_4}" "${SIG_4}"
	./make_load_test.sh ${OP}2_stack_overflow invalid "${OP}.2\n${OP}.2\n${OP}.2\n${OP}.2\n${OP}.2\n${OP}.2\n${OP}.2\n${OP}.2\n${OP}.2\n${POPS}" "${ARGS_4}" "${LOCALS_4}" "${CALL_4}" "${SIG_4}"
	./make_load_test.sh ${OP}3_stack_overflow invalid "${OP}.3\n${OP}.3\n${OP}.3\n${OP}.3\n${OP}.3\n${OP}.3\n${OP}.3\n${OP}.3\n${OP}.3\n${POPS}" "${ARGS_4}" "${LOCALS_4}" "${CALL_4}" "${SIG_4}"
done

#Test if the values used for brtrue and brfalse are valid
I=1
for OP in brfalse brtrue 'brfalse.s' 'brtrue.s'
do
	./make_bool_branch_test.sh boolean_branch_${I}_1 valid ${OP} int8
	./make_bool_branch_test.sh boolean_branch_${I}_2 valid ${OP} int16
	./make_bool_branch_test.sh boolean_branch_${I}_3 valid ${OP} int32
	./make_bool_branch_test.sh boolean_branch_${I}_4 valid ${OP} int64
	./make_bool_branch_test.sh boolean_branch_${I}_5 valid ${OP} 'native int'
	
	#unmanaged pointers are not veriable types, all ops on unmanaged pointers are unverifiable
	./make_bool_branch_test.sh boolean_branch_${I}_6 unverifiable ${OP} 'int32*'
	./make_bool_branch_test.sh boolean_branch_${I}_8 unverifiable ${OP} 'method int32 *(int32)'

	./make_bool_branch_test.sh boolean_branch_${I}_7 valid ${OP} 'int32&'
	./make_bool_branch_test.sh boolean_branch_${I}_9 valid ${OP} object
	./make_bool_branch_test.sh boolean_branch_${I}_10 valid ${OP} string
	./make_bool_branch_test.sh boolean_branch_${I}_11 valid ${OP} 'ClassA'
	./make_bool_branch_test.sh boolean_branch_${I}_12 valid ${OP} 'int32[]'
	./make_bool_branch_test.sh boolean_branch_${I}_13 valid ${OP} 'int32[,,]'
	./make_bool_branch_test.sh boolean_branch_${I}_14 valid ${OP} 'class Template`1<object>'
	./make_bool_branch_test.sh boolean_branch_${I}_15 valid ${OP} 'class Template`1<object>[]'
	./make_bool_branch_test.sh boolean_branch_${I}_16 valid ${OP} 'class Template`1<object>[,,]'
	
	./make_bool_branch_test.sh boolean_branch_${I}_17 unverifiable ${OP} float32
	./make_bool_branch_test.sh boolean_branch_${I}_18 unverifiable ${OP} float64
	./make_bool_branch_test.sh boolean_branch_${I}_19 unverifiable ${OP} 'class MyValueType'
	./make_bool_branch_test.sh boolean_branch_${I}_20 unverifiable ${OP} 'class ValueTypeTemplate`1<object>'
	I=`expr $I + 1`
done

#tests for field loading
I=1
for OP in 'ldfld' 'ldflda'
do
	./make_field_store_test.sh field_store_${I}_1 unverifiable "${OP} int32 ClassA::fld" int32 int32
	./make_field_store_test.sh field_store_${I}_2 unverifiable "${OP} int32 ClassA::fld" int32 'class ClassB' yes
	./make_field_store_test.sh field_store_${I}_3 unverifiable "${OP} int32 ClassA::fld" int32 object yes
	./make_field_store_test.sh field_store_${I}_4 unverifiable "${OP} int32 ClassA::fld" int32 'class MyValueType'
	./make_field_store_test.sh field_store_${I}_5 valid "${OP} int32 ClassA::fld" int32 'class ClassA' yes
	./make_field_store_test.sh field_store_${I}_6 valid "${OP} int32 ClassA::fld" int32 'class SubClass' yes
	#ldfld and ldflda works diferent with value objects, you cannot take the address of a value-object on the stack
	#./make_field_store_test.sh field_store_${I}_7 valid "${OP} int32 MyValueType::fld" int32 'class MyValueType'
	#Not usefull as it throws NRE
	#./make_field_store_test.sh field_store_${I}_8 valid "${OP} int32 MyValueType::fld" int32 'class MyValueType \&'
	./make_field_store_test.sh field_store_${I}_9 unverifiable "${OP} int32 MyValueType::fld" int32 'native int'
	./make_field_store_test.sh field_store_${I}_10 unverifiable "${OP} int32 MyValueType::fld" int32 'class MyValueType *'
	./make_field_store_test.sh field_store_${I}_11 unverifiable "${OP} int32 ClassA::fld" int32 'class ClassA *'
	./make_field_store_test.sh field_store_${I}_12 valid "${OP} int32 Overlapped::field1" int32 'class Overlapped' yes
	./make_field_store_test.sh field_store_${I}_13 unverifiable "${OP} ClassA Overlapped::field1" 'class ClassA' 'class Overlapped' yes
	./make_field_store_test.sh field_store_${I}_14 valid "${OP} int32 Overlapped::field1" int32 'class SubOverlapped' yes
	./make_field_store_test.sh field_store_${I}_15 unverifiable "${OP} ClassA Overlapped::field1" 'class ClassA' 'class SubOverlapped' yes
	./make_field_store_test.sh field_store_${I}_16 valid "${OP} int32 SubOverlapped::field6" int32 'class SubOverlapped' yes
	./make_field_store_test.sh field_store_${I}_17 unverifiable "${OP} ClassA SubOverlapped::field6" 'class ClassA' 'class SubOverlapped' yes
	./make_field_store_test.sh field_store_${I}_18 valid "${OP} int32 Overlapped::field10" int32 'class Overlapped' yes
	./make_field_store_test.sh field_store_${I}_20 unverifiable "${OP} int32 Overlapped::field10" 'class ClassA' 'class Overlapped' yes

	./make_field_store_test.sh field_store_${I}_22 invalid "${OP} int32 ClassA::unknown_field" 'class ClassA' 'class ClassA' yes
	./make_field_store_test.sh field_store_${I}_23 unverifiable "${OP} int32 ClassA::const_field" int32 'int32 \&'

	./make_field_store_test.sh field_store_${I}_24 valid "${OP} int32 ClassA::sfld" int32 'class ClassA' yes
	I=`expr $I + 1`
done

./make_field_store_test.sh field_store_2_25 unverifiable 'ldflda int32 ClassA::const_field' int32 'class ClassA'

#tests form static field loading
I=1
for OP in 'ldsfld' 'ldsflda'
do
	#unknown field
	./make_field_store_test.sh static_field_store_${I}_1 invalid "${OP} int32 ClassA::unknown_field\n\tpop" 'class ClassA' 'class ClassA'
	#non static field
	./make_field_store_test.sh static_field_store_${I}_2 invalid "${OP} int32 ClassA::fld\n\tpop" 'class ClassA' 'class ClassA'
	#valid
	./make_field_store_test.sh static_field_store_${I}_3 valid "${OP} ClassA ClassA::sfld\n\tpop" 'class ClassA' 'class ClassA'
	I=`expr $I + 1`
done

./make_field_store_test.sh static_field_store_2_25 unverifiable 'ldsflda int32 ClassA::st_const_field\n\tpop' int32 'class ClassA'

./make_field_valuetype_test.sh value_type_field_load_1 valid 'ldfld int32 MyValueType::fld' 'ldloc.0'
./make_field_valuetype_test.sh value_type_field_load_2 unverifiable 'ldflda int32 MyValueType::fld' 'ldloc.0'
./make_field_valuetype_test.sh value_type_field_load_3 valid 'ldfld int32 MyValueType::fld' 'ldloca.s 0'
./make_field_valuetype_test.sh value_type_field_load_4 valid 'ldflda int32 MyValueType::fld' 'ldloca.s 0'

./make_field_valuetype_test.sh value_type_field_load_1 valid 'ldfld int32 MyValueType::fld' 'ldloc.1'
./make_field_valuetype_test.sh value_type_field_load_2 valid 'ldflda int32 MyValueType::fld' 'ldloc.1'
./make_field_valuetype_test.sh value_type_field_load_3 unverifiable 'ldfld int32 MyValueType::fld' 'ldloca.s 1'
./make_field_valuetype_test.sh value_type_field_load_4 unverifiable 'ldflda int32 MyValueType::fld' 'ldloca.s 1'



#Tests for access checks
#TODO tests with static calls
#TODO tests with multiple assemblies, involving friend assemblies, with and without matching public key

I=1
for OP in "callvirt instance int32 class Owner\/Nested::Target()" "call instance int32 class Owner\/Nested::Target()" "ldc.i4.0\n\t\tstfld int32 Owner\/Nested::fld\n\t\tldc.i4.0" "ldc.i4.0\n\t\tstsfld int32 Owner\/Nested::sfld" "ldsfld int32 Owner\/Nested::sfld\n\n\tpop" "ldfld int32 Owner\/Nested::fld" "ldsflda int32 Owner\/Nested::sfld\n\n\tpop" "ldflda int32 Owner\/Nested::fld"
do
	./make_nested_access_test.sh nested_access_check_1_${I} valid "$OP" public public no
	./make_nested_access_test.sh nested_access_check_2_${I} valid "$OP" public public yes
	./make_nested_access_test.sh nested_access_check_3_${I} unverifiable "$OP" public private no
	./make_nested_access_test.sh nested_access_check_4_${I} unverifiable "$OP" public private yes
	./make_nested_access_test.sh nested_access_check_5_${I} unverifiable "$OP" public family no
	./make_nested_access_test.sh nested_access_check_6_${I} unverifiable "$OP" public family yes
	./make_nested_access_test.sh nested_access_check_7_${I} valid "$OP" public assembly no
	./make_nested_access_test.sh nested_access_check_8_${I} valid "$OP" public assembly yes
	./make_nested_access_test.sh nested_access_check_9_${I} unverifiable "$OP" public famandassem no
	./make_nested_access_test.sh nested_access_check_a_${I} unverifiable "$OP" public famandassem yes
	./make_nested_access_test.sh nested_access_check_b_${I} valid "$OP" public famorassem no
	./make_nested_access_test.sh nested_access_check_c_${I} valid "$OP" public famorassem yes

	./make_nested_access_test.sh nested_access_check_11_${I} unverifiable "$OP" private public no
	./make_nested_access_test.sh nested_access_check_12_${I} unverifiable "$OP" private public yes
	./make_nested_access_test.sh nested_access_check_13_${I} unverifiable "$OP" private private no
	./make_nested_access_test.sh nested_access_check_14_${I} unverifiable "$OP" private private yes
	./make_nested_access_test.sh nested_access_check_15_${I} unverifiable "$OP" private family no
	./make_nested_access_test.sh nested_access_check_16_${I} unverifiable "$OP" private family yes
	./make_nested_access_test.sh nested_access_check_17_${I} unverifiable "$OP" private assembly no
	./make_nested_access_test.sh nested_access_check_18_${I} unverifiable "$OP" private assembly yes
	./make_nested_access_test.sh nested_access_check_19_${I} unverifiable "$OP" private famandassem no
	./make_nested_access_test.sh nested_access_check_1a_${I} unverifiable "$OP" private famandassem yes
	./make_nested_access_test.sh nested_access_check_1b_${I} unverifiable "$OP" private famorassem no
	./make_nested_access_test.sh nested_access_check_1c_${I} unverifiable "$OP" private famorassem yes

	./make_nested_access_test.sh nested_access_check_21_${I} unverifiable "$OP" family public no
	./make_nested_access_test.sh nested_access_check_22_${I} valid "$OP" family public yes
	./make_nested_access_test.sh nested_access_check_23_${I} unverifiable "$OP" family private no
	./make_nested_access_test.sh nested_access_check_24_${I} unverifiable "$OP" family private yes
	./make_nested_access_test.sh nested_access_check_25_${I} unverifiable "$OP" family family no
	./make_nested_access_test.sh nested_access_check_26_${I} unverifiable "$OP" family family yes
	./make_nested_access_test.sh nested_access_check_27_${I} unverifiable "$OP" family assembly no
	./make_nested_access_test.sh nested_access_check_28_${I} valid "$OP" family assembly yes
	./make_nested_access_test.sh nested_access_check_29_${I} unverifiable "$OP" family famandassem no
	./make_nested_access_test.sh nested_access_check_2a_${I} unverifiable "$OP" family famandassem yes
	./make_nested_access_test.sh nested_access_check_2b_${I} unverifiable "$OP" family famorassem no
	./make_nested_access_test.sh nested_access_check_2c_${I} valid "$OP" family famorassem yes

	./make_nested_access_test.sh nested_access_check_31_${I} valid "$OP" assembly public no
	./make_nested_access_test.sh nested_access_check_32_${I} valid "$OP" assembly public yes
	./make_nested_access_test.sh nested_access_check_33_${I} unverifiable "$OP" assembly private no
	./make_nested_access_test.sh nested_access_check_34_${I} unverifiable "$OP" assembly private yes
	./make_nested_access_test.sh nested_access_check_35_${I} unverifiable "$OP" assembly family no
	./make_nested_access_test.sh nested_access_check_36_${I} unverifiable "$OP" assembly family yes
	./make_nested_access_test.sh nested_access_check_37_${I} valid "$OP" assembly assembly no
	./make_nested_access_test.sh nested_access_check_38_${I} valid "$OP" assembly assembly yes
	./make_nested_access_test.sh nested_access_check_39_${I} unverifiable "$OP" assembly famandassem no
	./make_nested_access_test.sh nested_access_check_3a_${I} unverifiable "$OP" assembly famandassem yes
	./make_nested_access_test.sh nested_access_check_3b_${I} valid "$OP" assembly famorassem no
	./make_nested_access_test.sh nested_access_check_3c_${I} valid "$OP" assembly famorassem yes

	./make_nested_access_test.sh nested_access_check_41_${I} unverifiable "$OP" famandassem public no
	./make_nested_access_test.sh nested_access_check_42_${I} valid "$OP" famandassem public yes
	./make_nested_access_test.sh nested_access_check_43_${I} unverifiable "$OP" famandassem private no
	./make_nested_access_test.sh nested_access_check_44_${I} unverifiable "$OP" famandassem private yes
	./make_nested_access_test.sh nested_access_check_45_${I} unverifiable "$OP" famandassem family no
	./make_nested_access_test.sh nested_access_check_46_${I} unverifiable "$OP" famandassem family yes
	./make_nested_access_test.sh nested_access_check_47_${I} unverifiable "$OP" famandassem assembly no
	./make_nested_access_test.sh nested_access_check_48_${I} valid "$OP" famandassem assembly yes
	./make_nested_access_test.sh nested_access_check_49_${I} unverifiable "$OP" famandassem famandassem no
	./make_nested_access_test.sh nested_access_check_4a_${I} unverifiable "$OP" famandassem famandassem yes
	./make_nested_access_test.sh nested_access_check_4b_${I} unverifiable "$OP" famandassem famorassem no
	./make_nested_access_test.sh nested_access_check_4c_${I} valid "$OP" famandassem famorassem yes

	./make_nested_access_test.sh nested_access_check_51_${I} valid "$OP" famorassem public no
	./make_nested_access_test.sh nested_access_check_52_${I} valid "$OP" famorassem public yes
	./make_nested_access_test.sh nested_access_check_53_${I} unverifiable "$OP" famorassem private no
	./make_nested_access_test.sh nested_access_check_54_${I} unverifiable "$OP" famorassem private yes
	./make_nested_access_test.sh nested_access_check_55_${I} unverifiable "$OP" famorassem family no
	./make_nested_access_test.sh nested_access_check_56_${I} unverifiable "$OP" famorassem family yes
	./make_nested_access_test.sh nested_access_check_57_${I} valid "$OP" famorassem assembly no
	./make_nested_access_test.sh nested_access_check_58_${I} valid "$OP" famorassem assembly yes
	./make_nested_access_test.sh nested_access_check_59_${I} unverifiable "$OP" famorassem famandassem no
	./make_nested_access_test.sh nested_access_check_5a_${I} unverifiable "$OP" famorassem famandassem yes
	./make_nested_access_test.sh nested_access_check_5b_${I} valid "$OP" famorassem famorassem no
	./make_nested_access_test.sh nested_access_check_5c_${I} valid "$OP" famorassem famorassem yes
	I=`expr $I + 1`
done

#Tests for accessing an owned nested type
I=1
for OP in "callvirt instance int32 class Outer\/Inner::Target()" "call instance int32 class Outer\/Inner::Target()" "ldc.i4.0\n\t\tstfld int32 Outer\/Inner::fld\n\t\tldc.i4.0" "ldc.i4.0\n\t\tstsfld int32 Outer\/Inner::sfld" "ldsfld int32 Outer\/Inner::sfld\n\n\tpop" "ldfld int32 Outer\/Inner::fld" "ldsflda int32 Outer\/Inner::sfld\n\n\tpop" "ldflda int32 Outer\/Inner::fld"
do
	./make_self_nested_test.sh self_nested_access_check_1_${I} valid "$OP" public public
	./make_self_nested_test.sh self_nested_access_check_2_${I} unverifiable "$OP" public private
	./make_self_nested_test.sh self_nested_access_check_3_${I} unverifiable "$OP" public family
	./make_self_nested_test.sh self_nested_access_check_4_${I} valid "$OP" public assembly
	./make_self_nested_test.sh self_nested_access_check_5_${I} unverifiable "$OP" public famandassem
	./make_self_nested_test.sh self_nested_access_check_6_${I} valid "$OP" public famorassem

	./make_self_nested_test.sh self_nested_access_check_7_${I} valid "$OP" private public
	./make_self_nested_test.sh self_nested_access_check_8_${I} unverifiable "$OP" private private
	./make_self_nested_test.sh self_nested_access_check_9_${I} unverifiable "$OP" private family
	./make_self_nested_test.sh self_nested_access_check_10_${I} valid "$OP" private assembly
	./make_self_nested_test.sh self_nested_access_check_11_${I} unverifiable "$OP" private famandassem
	./make_self_nested_test.sh self_nested_access_check_12_${I} valid "$OP" private famorassem

	./make_self_nested_test.sh self_nested_access_check_13_${I} valid "$OP" family public
	./make_self_nested_test.sh self_nested_access_check_14_${I} unverifiable "$OP" family private
	./make_self_nested_test.sh self_nested_access_check_15_${I} unverifiable "$OP" family family
	./make_self_nested_test.sh self_nested_access_check_16_${I} valid "$OP" family assembly
	./make_self_nested_test.sh self_nested_access_check_17_${I} unverifiable "$OP" family famandassem
	./make_self_nested_test.sh self_nested_access_check_18_${I} valid "$OP" family famorassem

	./make_self_nested_test.sh self_nested_access_check_19_${I} valid "$OP" assembly public
	./make_self_nested_test.sh self_nested_access_check_20_${I} unverifiable "$OP" assembly private
	./make_self_nested_test.sh self_nested_access_check_21_${I} unverifiable "$OP" assembly family
	./make_self_nested_test.sh self_nested_access_check_22_${I} valid "$OP" assembly assembly
	./make_self_nested_test.sh self_nested_access_check_23_${I} unverifiable "$OP" assembly famandassem
	./make_self_nested_test.sh self_nested_access_check_24_${I} valid "$OP" assembly famorassem

	./make_self_nested_test.sh self_nested_access_check_25_${I} valid "$OP" famandassem public
	./make_self_nested_test.sh self_nested_access_check_26_${I} unverifiable "$OP" famandassem private
	./make_self_nested_test.sh self_nested_access_check_27_${I} unverifiable "$OP" famandassem family
	./make_self_nested_test.sh self_nested_access_check_28_${I} valid "$OP" famandassem assembly
	./make_self_nested_test.sh self_nested_access_check_29_${I} valid "$unverifiable" famandassem famandassem
	./make_self_nested_test.sh self_nested_access_check_30_${I} valid "$OP" famandassem famorassem

	./make_self_nested_test.sh self_nested_access_check_31_${I} valid "$OP" famorassem public
	./make_self_nested_test.sh self_nested_access_check_32_${I} unverifiable "$OP" famorassem private
	./make_self_nested_test.sh self_nested_access_check_33_${I} unverifiable "$OP" famorassem family
	./make_self_nested_test.sh self_nested_access_check_34_${I} valid "$OP" famorassem assembly
	./make_self_nested_test.sh self_nested_access_check_35_${I} unverifiable "$OP" famorassem famandassem
	./make_self_nested_test.sh self_nested_access_check_36_${I} valid "$OP" famorassem famorassem
	I=`expr $I + 1`
done


I=1
for OP in "ldc.i4.0\n\t\tstsfld int32 Owner\/Nested::sfld" "ldsfld int32 Owner\/Nested::sfld\n\n\tpop" "ldsflda int32 Owner\/Nested::sfld\n\n\tpop" "callvirt instance int32 class Owner\/Nested::Target()" "call instance int32 class Owner\/Nested::Target()" "ldc.i4.0\n\t\tstfld int32 Owner\/Nested::fld\n\t\tldc.i4.0" "ldfld int32 Owner\/Nested::fld" "ldflda int32 Owner\/Nested::fld"
do
	./make_cross_nested_access_test.sh cross_nested_access_check_1_${I} valid "$OP" public public no
	./make_cross_nested_access_test.sh cross_nested_access_check_2_${I} valid "$OP" public public yes
	./make_cross_nested_access_test.sh cross_nested_access_check_3_${I} unverifiable "$OP" public private no
	./make_cross_nested_access_test.sh cross_nested_access_check_4_${I} unverifiable "$OP" public private yes
	./make_cross_nested_access_test.sh cross_nested_access_check_5_${I} unverifiable "$OP" public family no
	./make_cross_nested_access_test.sh cross_nested_access_check_7_${I} valid "$OP" public assembly no
	./make_cross_nested_access_test.sh cross_nested_access_check_8_${I} valid "$OP" public assembly yes
	./make_cross_nested_access_test.sh cross_nested_access_check_9_${I} unverifiable "$OP" public famandassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_b_${I} valid "$OP" public famorassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_c_${I} valid "$OP" public famorassem yes

	./make_cross_nested_access_test.sh cross_nested_access_check_11_${I} valid "$OP" private public no
	./make_cross_nested_access_test.sh cross_nested_access_check_12_${I} valid "$OP" private public yes
	./make_cross_nested_access_test.sh cross_nested_access_check_13_${I} unverifiable "$OP" private private no
	./make_cross_nested_access_test.sh cross_nested_access_check_14_${I} unverifiable "$OP" private private yes
	./make_cross_nested_access_test.sh cross_nested_access_check_15_${I} unverifiable "$OP" private family no
	./make_cross_nested_access_test.sh cross_nested_access_check_17_${I} valid "$OP" private assembly no
	./make_cross_nested_access_test.sh cross_nested_access_check_18_${I} valid "$OP" private assembly yes
	./make_cross_nested_access_test.sh cross_nested_access_check_19_${I} unverifiable "$OP" private famandassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_1b_${I} valid "$OP" private famorassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_1c_${I} valid "$OP" private famorassem yes

	./make_cross_nested_access_test.sh cross_nested_access_check_21_${I} valid "$OP" family public no
	./make_cross_nested_access_test.sh cross_nested_access_check_22_${I} valid "$OP" family public yes
	./make_cross_nested_access_test.sh cross_nested_access_check_23_${I} unverifiable "$OP" family private no
	./make_cross_nested_access_test.sh cross_nested_access_check_24_${I} unverifiable "$OP" family private yes
	./make_cross_nested_access_test.sh cross_nested_access_check_25_${I} unverifiable "$OP" family family no
	./make_cross_nested_access_test.sh cross_nested_access_check_27_${I} valid "$OP" family assembly no
	./make_cross_nested_access_test.sh cross_nested_access_check_28_${I} valid "$OP" family assembly yes
	./make_cross_nested_access_test.sh cross_nested_access_check_29_${I} unverifiable "$OP" family famandassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_2b_${I} valid "$OP" family famorassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_2c_${I} valid "$OP" family famorassem yes

	./make_cross_nested_access_test.sh cross_nested_access_check_31_${I} valid "$OP" assembly public no
	./make_cross_nested_access_test.sh cross_nested_access_check_32_${I} valid "$OP" assembly public yes
	./make_cross_nested_access_test.sh cross_nested_access_check_33_${I} unverifiable "$OP" assembly private no
	./make_cross_nested_access_test.sh cross_nested_access_check_34_${I} unverifiable "$OP" assembly private yes
	./make_cross_nested_access_test.sh cross_nested_access_check_35_${I} unverifiable "$OP" assembly family no
	./make_cross_nested_access_test.sh cross_nested_access_check_37_${I} valid "$OP" assembly assembly no
	./make_cross_nested_access_test.sh cross_nested_access_check_38_${I} valid "$OP" assembly assembly yes
	./make_cross_nested_access_test.sh cross_nested_access_check_39_${I} unverifiable "$OP" assembly famandassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_3b_${I} valid "$OP" assembly famorassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_3c_${I} valid "$OP" assembly famorassem yes

	./make_cross_nested_access_test.sh cross_nested_access_check_41_${I} valid "$OP" famandassem public no
	./make_cross_nested_access_test.sh cross_nested_access_check_42_${I} valid "$OP" famandassem public yes
	./make_cross_nested_access_test.sh cross_nested_access_check_43_${I} unverifiable "$OP" famandassem private no
	./make_cross_nested_access_test.sh cross_nested_access_check_44_${I} unverifiable "$OP" famandassem private yes
	./make_cross_nested_access_test.sh cross_nested_access_check_45_${I} unverifiable "$OP" famandassem family no
	./make_cross_nested_access_test.sh cross_nested_access_check_47_${I} valid "$OP" famandassem assembly no
	./make_cross_nested_access_test.sh cross_nested_access_check_48_${I} valid "$OP" famandassem assembly yes
	./make_cross_nested_access_test.sh cross_nested_access_check_49_${I} unverifiable "$OP" famandassem famandassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_4b_${I} valid "$OP" famandassem famorassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_4c_${I} valid "$OP" famandassem famorassem yes

	./make_cross_nested_access_test.sh cross_nested_access_check_51_${I} valid "$OP" famorassem public no
	./make_cross_nested_access_test.sh cross_nested_access_check_52_${I} valid "$OP" famorassem public yes
	./make_cross_nested_access_test.sh cross_nested_access_check_53_${I} unverifiable "$OP" famorassem private no
	./make_cross_nested_access_test.sh cross_nested_access_check_54_${I} unverifiable "$OP" famorassem private yes
	./make_cross_nested_access_test.sh cross_nested_access_check_55_${I} unverifiable "$OP" famorassem family no
	./make_cross_nested_access_test.sh cross_nested_access_check_57_${I} valid "$OP" famorassem assembly no
	./make_cross_nested_access_test.sh cross_nested_access_check_58_${I} valid "$OP" famorassem assembly yes
	./make_cross_nested_access_test.sh cross_nested_access_check_59_${I} unverifiable "$OP" famorassem famandassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_5b_${I} valid "$OP" famorassem famorassem no
	./make_cross_nested_access_test.sh cross_nested_access_check_5c_${I} valid "$OP" famorassem famorassem yes
	I=`expr $I + 1`
done


I=1
for OP in "callvirt instance int32 class Owner\/Nested::Target()" "call instance int32 class Owner\/Nested::Target()" "ldc.i4.0\n\t\tstfld int32 Owner\/Nested::fld\n\t\tldc.i4.0" "ldc.i4.0\n\t\tstsfld int32 Owner\/Nested::sfld" "ldsfld int32 Owner\/Nested::sfld\n\n\tpop" "ldfld int32 Owner\/Nested::fld" "ldsflda int32 Owner\/Nested::sfld\n\n\tpop" "ldflda int32 Owner\/Nested::fld"
do
	./make_cross_nested_access_test.sh cross_nested_access_check_2a_${I} valid "$OP" public public yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_4a_${I} unverifiable "$OP" public private yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_8a_${I} valid "$OP" public assembly yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_ca_${I} valid "$OP" public famorassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_12a_${I} valid "$OP" private public yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_14a_${I} unverifiable "$OP" private private yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_18a_${I} valid "$OP" private assembly yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_1ca_${I} valid "$OP" private famorassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_22a_${I} valid "$OP" family public yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_24a_${I} unverifiable "$OP" family private yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_28a_${I} valid "$OP" family assembly yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_2ca_${I} valid "$OP" family famorassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_32a_${I} valid "$OP" assembly public yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_34a_${I} unverifiable "$OP" assembly private yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_38a_${I} valid "$OP" assembly assembly yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_3ca_${I} valid "$OP" assembly famorassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_42a_${I} valid "$OP" famandassem public yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_44a_${I} unverifiable "$OP" famandassem private yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_48a_${I} valid "$OP" famandassem assembly yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_4ca_${I} valid "$OP" famandassem famorassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_52a_${I} valid "$OP" famorassem public yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_54a_${I} unverifiable "$OP" famorassem private yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_58a_${I} valid "$OP" famorassem assembly yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_5ca_${I} valid "$OP" famorassem famorassem yes yes
	I=`expr $I + 1`
done


I=1
for OP in "callvirt instance int32 class Owner\/Nested::Target()" "call instance int32 class Owner\/Nested::Target()" "ldc.i4.0\n\t\tstfld int32 Owner\/Nested::fld\n\t\tldc.i4.0" "ldfld int32 Owner\/Nested::fld" "ldflda int32 Owner\/Nested::fld"
do
	./make_cross_nested_access_test.sh cross_nested_access_check_6_${I} unverifiable "$OP" public family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_6a_${I} valid "$OP" public family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_a_${I} unverifiable "$OP" public famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_aa_${I} valid "$OP" public famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_16_${I} unverifiable "$OP" private family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_16a_${I} valid "$OP" private family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_1a_${I} unverifiable "$OP" private famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_1aa_${I} valid "$OP" private famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_26_${I} unverifiable "$OP" family family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_26a_${I} valid "$OP" family family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_2a_${I} unverifiable "$OP" family famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_2aa_${I} valid "$OP" family famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_36_${I} unverifiable "$OP" assembly family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_36a_${I} valid "$OP" assembly family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_3a_${I} unverifiable "$OP" assembly famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_3aa_${I} valid "$OP" assembly famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_46_${I} unverifiable "$OP" famandassem family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_46a_${I} valid "$OP" famandassem family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_4a_${I} unverifiable "$OP" famandassem famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_4aa_${I} valid "$OP" famandassem famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_56_${I} unverifiable "$OP" famorassem family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_56a_${I} valid "$OP" famorassem family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_5a_${I} unverifiable "$OP" famorassem famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_5aa_${I} valid "$OP" famorassem famandassem yes yes

	I=`expr $I + 1`
done

for OP in "ldc.i4.0\n\t\tstsfld int32 Owner\/Nested::sfld" "ldsfld int32 Owner\/Nested::sfld\n\n\tpop" "ldsflda int32 Owner\/Nested::sfld\n\n\tpop"
do
	./make_cross_nested_access_test.sh cross_nested_access_check_6_${I} valid "$OP" public family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_6a_${I} valid "$OP" public family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_a_${I} valid "$OP" public famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_aa_${I} valid "$OP" public famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_16_${I} valid "$OP" private family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_16a_${I} valid "$OP" private family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_1a_${I} valid "$OP" private famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_1aa_${I} valid "$OP" private famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_26_${I} valid "$OP" family family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_26a_${I} valid "$OP" family family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_2a_${I} valid "$OP" family famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_2aa_${I} valid "$OP" family famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_36_${I} valid "$OP" assembly family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_36a_${I} valid "$OP" assembly family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_3a_${I} valid "$OP" assembly famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_3aa_${I} valid "$OP" assembly famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_46_${I} valid "$OP" famandassem family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_46a_${I} valid "$OP" famandassem family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_4a_${I} valid "$OP" famandassem famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_4aa_${I} valid "$OP" famandassem famandassem yes yes

	./make_cross_nested_access_test.sh cross_nested_access_check_56_${I} valid "$OP" famorassem family yes
	./make_cross_nested_access_test.sh cross_nested_access_check_56a_${I} valid "$OP" famorassem family yes yes
	./make_cross_nested_access_test.sh cross_nested_access_check_5a_${I} valid "$OP" famorassem famandassem yes
	./make_cross_nested_access_test.sh cross_nested_access_check_5aa_${I} valid "$OP" famorassem famandassem yes yes

	I=`expr $I + 1`
done



I=1
for OP in "ldc.i4.0\n\t\tstfld int32 Class::fld\n\t\tldc.i4.0" "ldc.i4.0\n\t\tstsfld int32 Class::sfld" "ldsfld int32 Class::sfld\n\n\tpop" "ldfld int32 Class::fld" "ldsflda int32 Class::sfld\n\n\tpop" "ldflda int32 Class::fld" "call instance int32 Class::Method()" "callvirt int32 Class::Method()"
do
	./make_access_test.sh access_check_1_${I} valid "$OP" public public no
	./make_access_test.sh access_check_2_${I} valid "$OP" public public yes
	./make_access_test.sh access_check_3_${I} unverifiable "$OP" public private no
	./make_access_test.sh access_check_4_${I} unverifiable "$OP" public private yes
	./make_access_test.sh access_check_5_${I} unverifiable "$OP" public family no
	./make_access_test.sh access_check_7_${I} valid "$OP" public assembly no
	./make_access_test.sh access_check_8_${I} valid "$OP" public assembly yes
	./make_access_test.sh access_check_9_${I} unverifiable "$OP" public famandassem no
	./make_access_test.sh access_check_b_${I} valid "$OP" public famorassem no
	./make_access_test.sh access_check_c_${I} valid "$OP" public famorassem yes

	./make_access_test.sh access_check_11_${I} valid "$OP" private public no
	./make_access_test.sh access_check_12_${I} valid "$OP" private public yes
	./make_access_test.sh access_check_13_${I} unverifiable "$OP" private private no
	./make_access_test.sh access_check_14_${I} unverifiable "$OP" private private yes
	./make_access_test.sh access_check_15_${I} unverifiable "$OP" private family no
	./make_access_test.sh access_check_17_${I} valid "$OP" private assembly no
	./make_access_test.sh access_check_18_${I} valid "$OP" private assembly yes
	./make_access_test.sh access_check_19_${I} unverifiable "$OP" private famandassem no
	./make_access_test.sh access_check_1b_${I} valid "$OP" private famorassem no
	./make_access_test.sh access_check_1c_${I} valid "$OP" private famorassem yes

	./make_access_test.sh access_check_31_${I} valid "$OP" " " public no
	./make_access_test.sh access_check_32_${I} valid "$OP" " " public yes
	./make_access_test.sh access_check_33_${I} unverifiable "$OP" " " private no
	./make_access_test.sh access_check_34_${I} unverifiable "$OP" " " private yes
	./make_access_test.sh access_check_35_${I} unverifiable "$OP" " " family no
	./make_access_test.sh access_check_37_${I} valid "$OP" " " assembly no
	./make_access_test.sh access_check_38_${I} valid "$OP" " " assembly yes
	./make_access_test.sh access_check_39_${I} unverifiable "$OP" " " famandassem no
	./make_access_test.sh access_check_3b_${I} valid "$OP" " " famorassem no
	./make_access_test.sh access_check_3c_${I} valid "$OP" " " famorassem yes

	I=`expr $I + 1`
done

#static members are diferent from instance members
I=1
for OP in "ldc.i4.0\n\t\tstsfld int32 Class::sfld" "ldsfld int32 Class::sfld\n\n\tpop" "ldsflda int32 Class::sfld\n\n\tpop" 
do
	./make_access_test.sh access_check_41_${I} valid "$OP" public family yes
	./make_access_test.sh access_check_42_${I} valid "$OP" public famandassem yes
	./make_access_test.sh access_check_43_${I} valid "$OP" private family yes
	./make_access_test.sh access_check_44_${I} valid "$OP" private famandassem yes
	./make_access_test.sh access_check_45_${I} valid "$OP" " " family yes
	./make_access_test.sh access_check_46_${I} valid "$OP" " " famandassem yes
	I=`expr $I + 1`
done

#try to access the base stuff directly
I=1
for OP in "ldc.i4.0\n\t\tstfld int32 Class::fld\n\t\tldc.i4.0" "ldfld int32 Class::fld" "ldflda int32 Class::fld" "call instance int32 Class::Method()" "callvirt int32 Class::Method()"
do
	./make_access_test.sh access_check_51_${I} unverifiable "$OP" public family yes
	./make_access_test.sh access_check_52_${I} unverifiable "$OP" public famandassem yes
	./make_access_test.sh access_check_53_${I} unverifiable "$OP" private family yes
	./make_access_test.sh access_check_54_${I} unverifiable "$OP" private famandassem yes
	./make_access_test.sh access_check_55_${I} unverifiable "$OP" " " family yes
	./make_access_test.sh access_check_56_${I} unverifiable "$OP" " " famandassem yes
	I=`expr $I + 1`
done

#try to access the subclass stuff
I=1
for OP in "ldc.i4.0\n\t\tstfld int32 Class::fld\n\t\tldc.i4.0" "ldfld int32 Class::fld" "ldflda int32 Class::fld" "call instance int32 Class::Method()" "callvirt int32 Class::Method()"
do
	./make_access_test.sh access_check_61_${I} valid "$OP" public family yes yes
	./make_access_test.sh access_check_62_${I} valid "$OP" public famandassem yes yes
	./make_access_test.sh access_check_63_${I} valid "$OP" private family yes yes
	./make_access_test.sh access_check_64_${I} valid "$OP" private famandassem yes yes
	./make_access_test.sh access_check_65_${I} valid "$OP" " " family yes yes
	./make_access_test.sh access_check_66_${I} valid "$OP" " " famandassem yes yes
	I=`expr $I + 1`
done


function create_nesting_test_same_result () {
  K=$1
  for BASE in yes no
  do
    for NESTED in yes no
      do
        for LOAD in yes no
        do
          if ! ( [ "$NESTED" == "no" ] && [ "$LOAD" == "yes" ] ) ; then
            ./make_double_nesting_test.sh double_nesting_access_check_${K}_$I $2 "$OP" $3 $4 $5 "$BASE" "$NESTED" "$LOAD"
            K=`expr $K + 1`
          fi
      done
    done
  done
}

function create_nesting_test_only_first_ok () {
  FIRST=$1
  K=$1
  for BASE in yes no
  do
    for NESTED in yes no
      do
        for LOAD in yes no
        do
          if ! ( [ "$NESTED" == "no" ] && [ "$LOAD" == "yes" ] ) ; then
	   EXPECT=unverifiable
           if [ "$FIRST" == "$K" ]; then
              EXPECT=valid
           fi
           ./make_double_nesting_test.sh double_nesting_access_check_${K}_$I $EXPECT "$OP" $2 $3 $4 "$BASE" "$NESTED" "$LOAD"
           K=`expr $K + 1`
         fi
     done
    done
  done
}

I=1

for OP in "callvirt instance int32 class Root\/Nested::Target()" "call instance int32 class Root\/Nested::Target()" "ldc.i4.0\n\t\tstfld int32 Root\/Nested::fld\n\t\tldc.i4.0" "ldfld int32 Root\/Nested::fld" "ldflda int32 Root\/Nested::fld"
do
  create_nesting_test_same_result 1 valid public assembly assembly

  ./make_double_nesting_test.sh double_nesting_access_check_7_$I valid "$OP" public assembly family yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_8_$I unverifiable "$OP" public assembly family yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_9_$I unverifiable "$OP" public assembly family yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_10_$I valid "$OP" public assembly family no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_11_$I unverifiable "$OP" public assembly family no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_12_$I unverifiable "$OP" public assembly family no no no

  ./make_double_nesting_test.sh double_nesting_access_check_13_$I valid "$OP" public assembly famandassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_14_$I unverifiable "$OP" public assembly famandassem yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_15_$I unverifiable "$OP" public assembly famandassem yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_16_$I valid "$OP" public assembly famandassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_17_$I unverifiable "$OP" public assembly famandassem no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_18_$I unverifiable "$OP" public assembly famandassem no no no

  create_nesting_test_same_result 19 valid public assembly famorassem
  create_nesting_test_same_result 25 unverifiable public assembly private
  create_nesting_test_same_result 31 valid public assembly public

  ./make_double_nesting_test.sh double_nesting_access_check_37_$I valid "$OP" public family assembly yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_38_$I valid "$OP" public family assembly yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_39_$I valid "$OP" public family assembly yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_40_$I unverifiable "$OP" public family assembly no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_41_$I unverifiable "$OP" public family assembly no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_42_$I unverifiable "$OP" public family assembly no no no

  create_nesting_test_only_first_ok 43 public family family
  create_nesting_test_only_first_ok 49 public family famandassem

  ./make_double_nesting_test.sh double_nesting_access_check_55_$I valid "$OP" public family famorassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_56_$I valid "$OP" public family famorassem yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_57_$I valid "$OP" public family famorassem yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_58_$I unverifiable "$OP" public family famorassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_59_$I unverifiable "$OP" public family famorassem no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_60_$I unverifiable "$OP" public family famorassem no no no

   create_nesting_test_same_result 61 unverifiable public family private

  ./make_double_nesting_test.sh double_nesting_access_check_67_$I valid "$OP" public family public yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_68_$I valid "$OP" public family public yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_69_$I valid "$OP" public family public yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_70_$I unverifiable "$OP" public family public no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_71_$I unverifiable "$OP" public family public no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_72_$I unverifiable "$OP" public family public no no no

  ./make_double_nesting_test.sh double_nesting_access_check_73_$I valid "$OP" public famandassem assembly yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_74_$I valid "$OP" public famandassem assembly yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_75_$I valid "$OP" public famandassem assembly yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_76_$I unverifiable "$OP" public famandassem assembly no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_77_$I unverifiable "$OP" public famandassem assembly no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_78_$I unverifiable "$OP" public famandassem assembly no no no

  create_nesting_test_only_first_ok 79  public famandassem family
  create_nesting_test_only_first_ok 85  public famandassem famandassem

  ./make_double_nesting_test.sh double_nesting_access_check_91_$I valid "$OP" public famandassem famorassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_92_$I valid "$OP" public famandassem famorassem yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_93_$I valid "$OP" public famandassem famorassem yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_94_$I unverifiable "$OP" public famandassem famorassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_95_$I unverifiable "$OP" public famandassem famorassem no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_96_$I unverifiable "$OP" public famandassem famorassem no no no

   create_nesting_test_same_result 97 unverifiable public famandassem private

  ./make_double_nesting_test.sh double_nesting_access_check_103_$I valid "$OP" public famandassem public yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_104_$I valid "$OP" public famandassem public yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_105_$I valid "$OP" public famandassem public yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_106_$I unverifiable "$OP" public famandassem public no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_107_$I unverifiable "$OP" public famandassem public no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_108_$I unverifiable "$OP" public famandassem public no no no

  create_nesting_test_same_result 109 valid public famorassem assembly

  ./make_double_nesting_test.sh double_nesting_access_check_115_$I valid "$OP" public famorassem family yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_116_$I unverifiable "$OP" public famorassem family yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_117_$I unverifiable "$OP" public famorassem family yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_118_$I valid "$OP" public famorassem family no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_119_$I unverifiable "$OP" public famorassem family no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_120_$I unverifiable "$OP" public famorassem family no no no

  ./make_double_nesting_test.sh double_nesting_access_check_121_$I valid "$OP" public famorassem famandassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_122_$I unverifiable "$OP" public famorassem famandassem yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_123_$I unverifiable "$OP" public famorassem famandassem yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_124_$I valid "$OP" public famorassem famandassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_125_$I unverifiable "$OP" public famorassem famandassem no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_126_$I unverifiable "$OP" public famorassem famandassem no no no

  create_nesting_test_same_result 127 valid public famorassem famorassem
  create_nesting_test_same_result 133 unverifiable public famorassem private
  create_nesting_test_same_result 139 valid public famorassem public
  create_nesting_test_same_result 145 unverifiable public private assembly
  create_nesting_test_same_result 151 unverifiable public private family
  create_nesting_test_same_result 157 unverifiable public private famandassem
  create_nesting_test_same_result 163 unverifiable public private famorassem
  create_nesting_test_same_result 169 unverifiable public private private
  create_nesting_test_same_result 175 unverifiable public private public
  create_nesting_test_same_result 181 valid public public assembly

  ./make_double_nesting_test.sh double_nesting_access_check_187_$I valid "$OP" public public family yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_188_$I unverifiable "$OP" public public family yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_189_$I unverifiable "$OP" public public family yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_190_$I valid "$OP" public public family no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_191_$I unverifiable "$OP" public public family no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_192_$I unverifiable "$OP" public public family no no no

  ./make_double_nesting_test.sh double_nesting_access_check_193_$I valid "$OP" public public famandassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_194_$I unverifiable "$OP" public public famandassem yes yes no
  ./make_double_nesting_test.sh double_nesting_access_check_195_$I unverifiable "$OP" public public famandassem yes no no
  ./make_double_nesting_test.sh double_nesting_access_check_196_$I valid "$OP" public public famandassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_197_$I unverifiable "$OP" public public famandassem no yes no
  ./make_double_nesting_test.sh double_nesting_access_check_198_$I unverifiable "$OP" public public famandassem no no no

  create_nesting_test_same_result 199 valid public public famorassem
  create_nesting_test_same_result 205 unverifiable public public private
  create_nesting_test_same_result 211 valid public public public
  I=`expr $I + 1`
done

function create_nesting_test_same_result_static () {
  K=$1
  for BASE in yes no
  do
    for NESTED in yes no
      do
        ./make_double_nesting_test.sh double_nesting_access_check_${K}_$I $2 "$OP" $3 $4 $5 "$BASE" "$NESTED" yes
        K=`expr $K + 1`
    done
  done
}

function create_nesting_test_strips_result_static () {
  K=$1
  for BASE in yes no
  do
    for NESTED in yes no
      do
        EXPECT=unverifiable
        if [ "$NESTED" == "yes" ]; then
          EXPECT=valid
        fi
        ./make_double_nesting_test.sh double_nesting_access_check_${K}_$I $EXPECT "$OP" $2 $3 $4 "$BASE" "$NESTED" yes
        K=`expr $K + 1`
    done
  done
}

for OP in "ldc.i4.0\n\t\tstsfld int32 Root\/Nested::sfld" "ldsfld int32 Root\/Nested::sfld\n\n\tpop" "ldsflda int32 Root\/Nested::sfld\n\n\tpop"
do
   create_nesting_test_same_result 1 valid public assembly assembly

  create_nesting_test_strips_result_static 5 public assembly family
  create_nesting_test_strips_result_static 9 public assembly family

  create_nesting_test_same_result 13 valid public assembly famorassem
  create_nesting_test_same_result 17 unverifiable public assembly private
  create_nesting_test_same_result 21 valid public assembly public

  ./make_double_nesting_test.sh double_nesting_access_check_25_$I valid "$OP" public family assembly yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_26_$I valid "$OP" public family assembly yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_27_$I unverifiable "$OP" public family assembly no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_27_$I unverifiable "$OP" public family assembly no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_29_$I valid "$OP" public family family yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_30_$I unverifiable "$OP" public family family yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_31_$I unverifiable "$OP" public family family no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_32_$I unverifiable "$OP" public family family no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_33_$I valid "$OP" public family famandassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_34_$I unverifiable "$OP" public family famandassem yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_35_$I unverifiable "$OP" public family famandassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_36_$I unverifiable "$OP" public family famandassem no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_37_$I valid "$OP" public family famorassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_38_$I valid "$OP" public family famorassem yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_39_$I unverifiable "$OP" public family famorassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_40_$I unverifiable "$OP" public family famorassem no no yes

   create_nesting_test_same_result 41 unverifiable public family private

  ./make_double_nesting_test.sh double_nesting_access_check_45_$I valid "$OP" public family public yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_46_$I valid "$OP" public family public yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_47_$I unverifiable "$OP" public family public no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_48_$I unverifiable "$OP" public family public no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_49_$I valid "$OP" public famandassem assembly yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_50_$I valid "$OP" public famandassem assembly yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_51_$I unverifiable "$OP" public famandassem assembly no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_52_$I unverifiable "$OP" public famandassem assembly no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_53_$I valid "$OP" public famandassem family yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_54_$I unverifiable "$OP" public famandassem family yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_55_$I unverifiable "$OP" public famandassem family no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_56_$I unverifiable "$OP" public famandassem family no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_57_$I valid "$OP" public famandassem famandassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_58_$I unverifiable "$OP" public famandassem famandassem yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_59_$I unverifiable "$OP" public famandassem famandassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_60_$I unverifiable "$OP" public famandassem famandassem no no yes

  ./make_double_nesting_test.sh double_nesting_access_check_61_$I valid "$OP" public famandassem famorassem yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_62_$I valid "$OP" public famandassem famorassem yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_63_$I unverifiable "$OP" public famandassem famorassem no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_64_$I unverifiable "$OP" public famandassem famorassem no no yes

  create_nesting_test_same_result 65 unverifiable public famandassem private

  ./make_double_nesting_test.sh double_nesting_access_check_69_$I valid "$OP" public famandassem public yes yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_70_$I valid "$OP" public famandassem public yes no yes
  ./make_double_nesting_test.sh double_nesting_access_check_71_$I unverifiable "$OP" public famandassem public no yes yes
  ./make_double_nesting_test.sh double_nesting_access_check_72_$I unverifiable "$OP" public famandassem public no no yes

  create_nesting_test_same_result 73 valid public famorassem assembly
  create_nesting_test_strips_result_static 77 public famorassem family
  create_nesting_test_strips_result_static 81 public famorassem famandassem

  create_nesting_test_same_result 85 valid public famorassem famorassem
  create_nesting_test_same_result 89 unverifiable public famorassem private
  create_nesting_test_same_result 93 valid public famorassem public
  create_nesting_test_same_result 97 unverifiable public private assembly
  create_nesting_test_same_result 101 unverifiable public private family
  create_nesting_test_same_result 105 unverifiable public private famandassem
  create_nesting_test_same_result 109 unverifiable public private famorassem
  create_nesting_test_same_result 113 unverifiable public private private
  create_nesting_test_same_result 117 unverifiable public private public
  create_nesting_test_same_result 121 valid public public assembly
  create_nesting_test_strips_result_static 125 public public family
  create_nesting_test_strips_result_static 129 public public famandassem
  create_nesting_test_same_result 133 valid public public famorassem
  create_nesting_test_same_result 137 unverifiable public public private
  create_nesting_test_same_result 141 valid public public public

  I=`expr $I + 1`
done


#ldtoken tests

./make_ldtoken_test.sh ldtoken_class valid "ldtoken class Example" "call class [mscorlib]System.Type class [mscorlib]System.Type::GetTypeFromHandle(valuetype [mscorlib]System.RuntimeTypeHandle)"

./make_ldtoken_test.sh ldtoken_class invalid "ldtoken class [mscorlib]ExampleMM" "call class [mscorlib]System.Type class [mscorlib]System.Type::GetTypeFromHandle(valuetype [mscorlib]System.RuntimeTypeHandle)"

./make_ldtoken_test.sh ldtoken_field valid "ldtoken field int32 Example::fld" "call class [mscorlib]System.Reflection.FieldInfo class [mscorlib]System.Reflection.FieldInfo::GetFieldFromHandle(valuetype [mscorlib]System.RuntimeFieldHandle)"

./make_ldtoken_test.sh ldtoken_field invalid "ldtoken field int32 Example::MM" "call class [mscorlib]System.Reflection.FieldInfo class [mscorlib]System.Reflection.FieldInfo::GetFieldFromHandle(valuetype [mscorlib]System.RuntimeFieldHandle)"

./make_ldtoken_test.sh ldtoken_method valid "ldtoken method void Example::Method()" "call class [mscorlib]System.Reflection.MethodBase class [mscorlib]System.Reflection.MethodBase::GetMethodFromHandle(valuetype [mscorlib]System.RuntimeMethodHandle)"

./make_ldtoken_test.sh ldtoken_method invalid "ldtoken method int32 Example::Method()" "call class [mscorlib]System.Reflection.MethodBase class [mscorlib]System.Reflection.MethodBase::GetMethodFromHandle(valuetype [mscorlib]System.RuntimeMethodHandle)"


#ldobj tests
function fix_ldobj () {
	if [ "$3" != "" ]; then
		A=$3;
	elif [ "$2" != "" ]; then
		A=$2;
	else
		A=$1;
	fi

	if [ "$A" == "bool" ]; then
		A="int8";
	elif [ "$A" == "char" ]; then
		A="int16";
	fi

	echo "$A";
}


I=1

#valid
for T1 in 'int8' 'bool' 'unsigned int8' 'int16' 'char' 'unsigned int16' 'int32' 'unsigned int32' 'int64' 'unsigned int64' 'float32' 'float64'
do
	for T2 in 'int8' 'bool' 'unsigned int8' 'int16' 'char' 'unsigned int16' 'int32' 'unsigned int32' 'int64' 'unsigned int64' 'float32' 'float64'
	do
		TYPE1="$(fix_ldobj $T1)"
		TYPE2="$(fix_ldobj $T2)"
		if [ "$TYPE1" == "$TYPE2" ] ; then
			./make_ldobj_test.sh ldobj_${I} valid "${T1}\&" "${T2}"
		else
			./make_ldobj_test.sh ldobj_${I} unverifiable "${T1}\&" "${T2}"
		fi
		I=`expr $I + 1`
	done
done



#unverifiable
#for T1 in "int8" "int64" "float64" "object" "string" "class Class" "int32[]" "int32[,]" "valuetype MyStruct" "valuetype MyStruct2" "int32 *" "valuetype MyStruct *" "method int32 *(int32)"
for T1 in "native int" "int8*" "typedref" 
do
	for T2 in "int8" "int64" "float64" "object" "string" "class Class" "int32[]" "int32[,]" "valuetype MyStruct" "valuetype MyStruct2"   "int32 *" "valuetype MyStruct *" "method int32 *(int32)" "native int"  "typedref" "typedref\&" "class Template\`1<object>" "valuetype StructTemplate\`1<object>" "valuetype StructTemplate2\`1<object>"
	do 
		./make_ldobj_test.sh ldobj_${I} unverifiable "${T1}" "${T2}"
		I=`expr $I + 1`
	done
done




#invalid
#for T1 in "int8" "int64" "float64" "object" "string" "class Class" "int32[]" "int32[,]" "valuetype MyStruct" "valuetype MyStruct2" "int32 *" "valuetype MyStruct *" "method int32 *(int32)"
for T1 in 'int8' 'native int'
do	
	for T2 in "int8\&" "int64\&" "float64\&" "object\&" "string\&" "class Class\&" "valuetype MyStruct\&" "native int\&" "class Template\`1<object>\&" "valuetype StructTemplate\`1<object>\&"  "valuetype StructTemplate2\`1<object>\&" "class [mscorlib]ExampleMM" "class [mscorlib]ExampleMM\&"
	do 
		./make_ldobj_test.sh ldobj_${I} invalid "${T1}" "${T2}"
		I=`expr $I + 1`
	done
done

./make_ldobj_test.sh ldobj_struct_1 valid  "valuetype MyStruct\&" "valuetype MyStruct"
./make_ldobj_test.sh ldobj_struct_2 unverifiable  "valuetype MyStruct\&" "valuetype MyStruct2"
./make_ldobj_test.sh ldobj_struct_3 valid  "valuetype StructTemplate\`1<object>\&" "valuetype StructTemplate\`1<object>"
./make_ldobj_test.sh ldobj_struct_4 unverifiable  "valuetype StructTemplate\`1<object>\&" "valuetype StructTemplate2\`1<object>"

./make_ldobj_test.sh ldobj_struct_5 valid  "object\&"  "object"
./make_ldobj_test.sh ldobj_struct_6 valid  "string\&"  "string"
./make_ldobj_test.sh ldobj_struct_7 valid  "int32[]\&"  "int32[]"
./make_ldobj_test.sh ldobj_struct_8 valid  "int32[,]\&"  "int32[,]"
./make_ldobj_test.sh ldobj_struct_9 valid  "class Template\`1<object>\&"  "class Template\`1<object>"


# Unbox Test


# unbox non-existent type.
./make_unbox_test.sh unbox_bad_type invalid "valuetype [mscorlib]NonExistent" "valuetype [mscorlib]NonExistent"

# Unbox byref type.
./make_unbox_test.sh unbox_byref_type invalid "int32" 'int32\&'

# Box unbox-like type.
./make_unbox_test.sh unbox_byref_like unverifiable int32 typedref

# Box unbox-like type.
./make_unbox_test.sh unbox_wrong_types valid object int32

#This is illegal since you cannot have a Void local variable, it should go into the structural tests part
# Box void type.
#./make_unary_test.sh box_void unverifiable "box [mscorlib]System.Void\n\tpop" "class [mscorlib]System.Void"
I=1;
for OP in "native int" "int32*" typedref int16 string float32
do
	./make_unbox_test.sh unbox_bad_stack_${I} unverifiable "${OP}" int32 "nop" "yes"
	I=`expr $I + 1`
done


#unboxing from int32
./make_unbox_test.sh unbox_wrong_types_1 unverifiable int32 int32 "nop" "yes"

#unboxing from valuetype
./make_unbox_test.sh unbox_wrong_types_2 unverifiable "valuetype MyStruct" int32 "nop" "yes"

#unboxing from managed ref
./make_unbox_test.sh unbox_stack_byref unverifiable "valuetype MyEnum\&" "valuetype MyEnum" "nop" "yes"

# valid unboxing
./make_unbox_test.sh unbox_primitive valid "int32" "int32"
./make_unbox_test.sh unbox_struct valid "valuetype MyStruct" 'valuetype MyStruct'
./make_unbox_test.sh unbox_template valid "valuetype StructTemplate\`1<object>" "valuetype StructTemplate\`1<object>"
./make_unbox_test.sh unbox_enum valid "valuetype MyEnum" "valuetype MyEnum"


#test if the unboxed value is right
./make_unbox_test.sh unbox_use_result_1 valid "valuetype MyStruct" "valuetype MyStruct" "ldfld int32 MyStruct::foo"
./make_unbox_test.sh unbox_use_result_2 valid "valuetype MyStruct" "valuetype MyStruct" "ldobj valuetype MyStruct\n\tstloc.0\n\tldc.i4.0"
./make_unbox_test.sh unbox_use_result_3 valid "int32" "int32" "ldind.i4\n\tstloc.1\n\tldc.i4.0"
