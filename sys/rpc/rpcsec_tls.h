/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2020 Rick Macklem
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$FreeBSD$
 */

#ifndef	_RPCTLS_IMPL_H
#define	_RPCTLS_IMPL_H

/* Operation values for rpctls syscall. */
#define	RPCTLS_SYSC_SETPATH	1
#define	RPCTLS_SYSC_CONNECT	2
#define	RPCTLS_SYSC_SERVER	3

/* Flag bits to indicate certificate results. */
#define	RPCTLS_FLAGS_HANDSHAKE	0x01
#define	RPCTLS_FLAGS_GOTCERT	0x02
#define	RPCTLS_FLAGS_SELFSIGNED	0x04
#define	RPCTLS_FLAGS_VERIFIED	0x08
#define	RPCTLS_FLAGS_DISABLED	0x10
#define	RPCTLS_FLAGS_CERTUSER	0x20

#ifdef _KERNEL
/* Functions that perform upcalls to the rpctlsd daemon. */
enum clnt_stat	rpctls_connect(CLIENT *newclient, struct socket *so,
		    uint64_t *sslp);
enum clnt_stat	rpctls_cl_disconnect(uint64_t sec, uint64_t usec, uint64_t ssl);
enum clnt_stat	rpctls_srv_disconnect(uint64_t sec, uint64_t usec,
		    uint64_t ssl);

/* Initialization function for rpcsec_tls. */
int		rpctls_init(void);

/* String for AUTH_TLS reply verifier. */
#define	RPCTLS_START_STRING	"STARTTLS"

#endif	/* _KERNEL */

#endif	/* _RPCTLS_IMPL_H */
