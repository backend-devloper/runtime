/*
 * networking.h: Portable networking functions
 *
 * Author:
 *	Rodrigo Kumpera (kumpera@gmail.com)
 *
 * (C) 2015 Xamarin
 */


#ifndef __MONO_NETWORKING_H__
#define __MONO_NETWORKING_H__

#include <config.h>

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HOST_WIN32
#include <winsock2.h>
#endif

#include <mono/utils/mono-compiler.h>

typedef enum {
	MONO_HINT_UNSPECIFIED		= 0,
	MONO_HINT_IPV4				= 1,
	MONO_HINT_IPV6				= 2,
	MONO_HINT_CANONICAL_NAME	= 4,
	MONO_HINT_CONFIGURED_ONLY	= 8,
} MonoGetAddressHints;

typedef struct _MonoAddressEntry MonoAddressEntry;

struct _MonoAddressEntry {
	int family;
	int socktype;
	int protocol;
	int address_len;
	union {
		struct in_addr v4;
		struct in6_addr v6;
	} address;
	const char *canonical_name;
	MonoAddressEntry *next;
};

typedef struct {
	MonoAddressEntry *entries;
	char **aliases;
} MonoAddressInfo;

typedef union {
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
	struct sockaddr addr;
} MonoSocketAddress;

/* This only supports IPV4 / IPV6 and tcp */
int mono_get_address_info (const char *hostname, int port, int flags, MonoAddressInfo **res) MONO_INTERNAL;

void mono_free_address_info (MonoAddressInfo *ai) MONO_INTERNAL;

void mono_socket_address_init (MonoSocketAddress *sa, socklen_t *len, int family, const void *address, int port) MONO_INTERNAL;

void *mono_get_local_interfaces (int family, int *interface_count) MONO_INTERNAL;

#ifndef HAVE_INET_PTON
int inet_pton (int family, const char *address, void *inaddrp) MONO_INTERNAL;
#endif

int mono_networking_get_tcp_protocol (void) MONO_INTERNAL;
int mono_networking_get_ip_protocol (void) MONO_INTERNAL;
int mono_networking_get_ipv6_protocol (void) MONO_INTERNAL;

#endif
