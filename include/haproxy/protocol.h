/*
 * include/haproxy/protocol.h
 * This file declares generic protocol management primitives.
 *
 * Copyright (C) 2000-2020 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_PROTOCOL_H
#define _HAPROXY_PROTOCOL_H

#include <sys/socket.h>
#include <haproxy/protocol-t.h>
#include <haproxy/thread.h>

/* [AF][sock_dgram][ctrl_dgram] */
extern struct protocol *__protocol_by_family[AF_CUST_MAX][2][2];
__decl_thread(extern HA_SPINLOCK_T proto_lock);

/* Registers the protocol <proto> */
void protocol_register(struct protocol *proto);

/* Unregisters the protocol <proto>. Note that all listeners must have
 * previously been unbound.
 */
void protocol_unregister(struct protocol *proto);

/* binds all listeners of all registered protocols. Returns a composition
 * of ERR_NONE, ERR_RETRYABLE, ERR_FATAL, ERR_ABORT.
 */
int protocol_bind_all(int verbose);

/* unbinds all listeners of all registered protocols. They are also closed.
 * This must be performed before calling exit() in order to get a chance to
 * remove file-system based sockets and pipes.
 * Returns a composition of ERR_NONE, ERR_RETRYABLE, ERR_FATAL.
 */
int protocol_unbind_all(void);

/* enables all listeners of all registered protocols. This is intended to be
 * used after a fork() to enable reading on all file descriptors. Returns a
 * composition of ERR_NONE, ERR_RETRYABLE, ERR_FATAL.
 */
int protocol_enable_all(void);

/* returns the protocol associated to family <family> with sock_type and
 * ctrl_type of SOCK_STREAM, or NULL if not found
 */
static inline struct protocol *protocol_by_family(int family)
{
	if (family >= 0 && family < AF_CUST_MAX)
		return __protocol_by_family[family][0][0];
	return NULL;
}

/* returns the protocol associated to family <family> with sock_type and
 * ctrl_type of either SOCK_STREAM or SOCK_DGRAM depending on the requested
 * values, or NULL if not found.
 */
static inline struct protocol *protocol_lookup(int family, int sock_dgram, int ctrl_dgram)
{
	if (family >= 0 && family < AF_CUST_MAX)
		return __protocol_by_family[family][!!sock_dgram][!!ctrl_dgram];
	return NULL;
}

#endif /* _HAPROXY_PROTOCOL_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
