/*
 * atomic.c:  Workarounds for atomic operations for platforms that dont have
 *	      really atomic asm functions in atomic.h
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <pthread.h>

#include "mono/io-layer/wapi.h"

#ifdef WAPI_ATOMIC_ASM
#if defined(sparc) || defined (__sparc__)
volatile unsigned char _wapi_sparc_lock;
#endif
#else

static pthread_mutex_t spin;
static mono_once_t spin_once=MONO_ONCE_INIT;

static void spin_init(void)
{
	pthread_mutex_init(&spin, 0);
	g_warning("Using non-atomic functions!");
}

gint32 InterlockedCompareExchange(volatile gint32 *dest, gint32 exch,
				  gint32 comp)
{
	gint32 old;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);
	
	old= *dest;
	if(old==comp) {
		*dest=exch;
	}
	
	pthread_mutex_unlock(&spin);

	return(old);
}

gpointer InterlockedCompareExchangePointer(volatile gpointer *dest,
					   gpointer exch, gpointer comp)
{
	gpointer old;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);
	
	old= *dest;
	if(old==comp) {
		*dest=exch;
	}
	
	pthread_mutex_unlock(&spin);

	return(old);
}

gint32 InterlockedIncrement(volatile gint32 *dest)
{
	gint32 ret;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);
	
	*dest++;
	ret= *dest;
	
	pthread_mutex_unlock(&spin);
	
	return(ret);
}

gint32 InterlockedDecrement(volatile gint32 *dest)
{
	gint32 ret;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);
	
	*dest--;
	ret= *dest;
	
	pthread_mutex_unlock(&spin);
	
	return(ret);
}

gint32 InterlockedExchange(volatile gint32 *dest, gint32 exch)
{
	gint32 ret;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);

	ret=*dest;
	*dest=exch;
	
	pthread_mutex_unlock(&spin);
	
	return(ret);
}

gpointer InterlockedExchangePointer(volatile gpointer *dest, gpointer exch)
{
	gpointer ret;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);
	
	ret=*dest;
	*dest=exch;
	
	pthread_mutex_unlock(&spin);
	
	return(ret);
}

gint32 InterlockedExchangeAdd(volatile gint32 *dest, gint32 add)
{
	gint32 ret;
	
	mono_once(&spin_once, spin_init);
	pthread_mutex_lock(&spin);

	ret= *dest;
	*dest+=add;
	
	pthread_mutex_unlock(&spin);
	
	return(ret);
}

#endif
