#include <stdio.h>

#include "test.h"

int main ()
{
	printf ("hashtable\n");
	test ("hash-1", hash_t1);
	test ("s-freev", test_strfreev);
	test ("s-concat", test_concat);
}
