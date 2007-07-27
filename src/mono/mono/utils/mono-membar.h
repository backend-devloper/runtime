/*
 * mono-membar.h: Memory barrier inline functions
 *
 * Author:
 *	Mark Probst (mark.probst@gmail.com)
 *
 * (C) 2007 Novell, Inc
 */

#ifndef _MONO_UTILS_MONO_MEMBAR_H_
#define _MONO_UTILS_MONO_MEMBAR_H_

#ifdef __x86_64__
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("mfence" : : : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	__asm__ __volatile__ ("lfence" : : : "memory");
}

static inline void mono_memory_write_barrier (void)
{
	__asm__ __volatile__ ("sfence" : : : "memory");
}
#elif defined(__i386__)
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("lock; addl $0,0(%%esp)" : : : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	mono_memory_barrier ();
}

static inline void mono_memory_write_barrier (void)
{
	mono_memory_barrier ();
}
#elif defined(sparc) || defined(__sparc__)
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("membar	#LoadLoad | #LoadStore | #StoreStore | #StoreLoad" : : : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	__asm__ __volatile__ ("membar	#LoadLoad" : : : "memory");
}

static inline void mono_memory_write_barrier (void)
{
	__asm__ __volatile__ ("membar	#StoreStore" : : : "memory");
}
#elif defined(__s390__)
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("bcr 15,0" : : : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	mono_memory_barrier ();
}

static inline void mono_memory_write_barrier (void)
{
	mono_memory_barrier ();
}
#elif defined(__ppc__) || defined(__powerpc__)
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("sync" : : : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	mono_memory_barrier ();
}

static inline void mono_memory_write_barrier (void)
{
	__asm__ __volatile__ ("eieio" : : : "memory");
}

#elif defined(__arm__)
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 5"
			      : : "r" (0) : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	mono_memory_barrier ();
}

static inline void mono_memory_write_barrier (void)
{
	mono_memory_barrier ();
}
#elif defined(__ia64__)
static inline void mono_memory_barrier (void)
{
	__asm__ __volatile__ ("mf" : : : "memory");
}

static inline void mono_memory_read_barrier (void)
{
	mono_memory_barrier ();
}

static inline void mono_memory_write_barrier (void)
{
	mono_memory_barrier ();
}
#endif

#endif	/* _MONO_UTILS_MONO_MEMBAR_H_ */
