/*
 * sort.frag.h: Common implementation of linked-list sorting
 *
 * Author:
 *   Raja R Harinath (rharinath@novell.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * (C) 2006 Novell, Inc.
 */

/*
 * This code requires a typedef named 'list_node' for the list node.  It
 * is assumed that the list type is the type of a pointer to a list
 * node, and that the node has a field named 'next' that implements to
 * the linked list.  No additional invariant is maintained (e.g. the
 * 'prev' pointer of a doubly-linked list node is _not_ updated).  Any
 * invariant would require a post-processing pass to fix matters if
 * necessary.
 *
 * Note: We refer to a list fragment as a "digit" because the code for
 * maintaining the invariants of the core data structure parallels the
 * code for incrementing the binary representation of a number.
 */
typedef list_node *digit;

/*
 * The maximum possible depth of the merge tree
 *   = ceiling (log2 (maximum number of list nodes))
 *   = ceiling (log2 (maximum possible memory size/size of each list node))
 *   = number of bits in 'size_t' - floor (log2 (sizeof digit))
 * Also, each "digit" is at least 2 nodes long: we can reduce the depth by 1
 */

#define FLOOR_LOG2(x) (((x)>=2) + ((x)>=4) + ((x)>=8) + ((x)>=16) + ((x)>=32) + ((x)>=64) + ((x)>=128))
#define MAX_DIGITS ((sizeof (size_t) * 8) - FLOOR_LOG2(sizeof (list_node)) - 1)

static inline digit
add_digits (digit first, digit second, GCompareFunc func)
{
	/* merge the two lists */
	digit list = NULL;
	digit *pos = &list;
	while (first && second) {
		if (func (first->data, second->data) > 0) {
			*pos = second;
			second = second->next;
		} else {
			*pos = first;
			first = first->next;
		}
		pos = &((*pos)->next);
	}
	*pos = first ? first : second;
	return list;
}

static inline digit
combine_digits (digit *digits, digit list, int n_digits, GCompareFunc func)
{
	int i;
	for (i = 0; i < n_digits; ++i)
		list = add_digits (digits [i], list, func);
	return list;
}

/*
 * Given: length(list) == k
 * Invariant: digit[i] == NULL || length(digit[i]) == k * 2**i
 */
static inline int
increment (digit *digits, digit list, int n_digits, GCompareFunc func)
{
	int i;
	for (i = 0; i < n_digits && digits [i]; i++) {
		list = add_digits (digits [i], list, func);
		digits [i] = NULL;
	}
	if (i == MAX_DIGITS) /* Will _never_ happen: so we can just devolve into quadratic ;-) */
		--i;

	if (i == n_digits)
		++n_digits;
	digits [i] = list;
	return n_digits;
}

/*
 * A mergesort that avoids recursion.  The 'digits' array essentially
 * captures the recursion stack.  The actual merge tree is built in a
 * bottom-up manner.  It's "counting", since we "increment" a set of
 * "digit"s.
 */
static inline digit
do_sort (digit list, GCompareFunc func)
{
	int n_digits = 0;
	digit digits [MAX_DIGITS]; /* ~ 128 bytes on 32bit, ~ 512 bytes on 64bit */

	while (list && list->next) {
		digit next = list->next;
		digit tail = next->next;

		if (func (list->data, next->data) > 0) {
			next->next = list;
			next = list;
			list = list->next;
		}
		next->next = NULL;

		n_digits = increment (digits, list, n_digits, func);

		list = tail;
	}

	return combine_digits (digits, list, n_digits, func);
}

#undef MAX_DIGITS
#undef FLOOR_LOG2
