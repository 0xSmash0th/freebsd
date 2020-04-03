/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2008 Isilon Inc http://www.isilon.com/
 * Authors: Doug Rabson <dfr@rabson.org>
 * Developed with Red Inc: Alfred Perlstein <alfred@freebsd.org>
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
 */

/* Modified from the kernel GSSAPI code for RPC-over-TLS. */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_kern_tls.h"

#include <sys/param.h>
#include <sys/capsicum.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/socketvar.h>
#include <sys/syscall.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>

#include <rpc/rpc.h>
#include <rpc/rpc_com.h>
#include <rpc/rpcsec_tls.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_param.h>

#include "rpctlscd.h"
#include "rpctlssd.h"

extern struct fileops badfileops;

/*
 * Syscall hooks
 */
static struct syscall_helper_data rpctls_syscalls[] = {
	SYSCALL_INIT_HELPER(gssd_syscall),
	SYSCALL_INIT_LAST
};

#ifdef notnow
struct rpctls_syscall_args {
	char op_l_[PADL_(int)]; int op; char op_r_[PADR_(int)];
	char path_l_[PADL_(const char *)]; const char * path; char path_r_[PADR_(const char *)];
	char s_l_[PADL_(int)]; int s; char s_r_[PADR_(int)];
};
#endif

static CLIENT		*rpctls_connect_handle;
static struct mtx	rpctls_connect_lock;
static struct socket	*rpctls_connect_so = NULL;
static CLIENT		*rpctls_server_handle;
static struct mtx	rpctls_server_lock;
static struct socket	*rpctls_server_so = NULL;
static struct opaque_auth rpctls_null_verf;

static CLIENT		*rpctls_connect_client(void);
static CLIENT		*rpctls_server_client(void);
static enum clnt_stat	rpctls_server(struct socket *so,
			    uint32_t *flags, uint64_t *sslp,
			    uid_t *uid, int *ngrps, gid_t **gids);

static void
rpctls_init(void *dummy)
{
	int error;

	error = syscall_helper_register(rpctls_syscalls, SY_THR_STATIC_KLD);
	if (error != 0)
		printf("rpctls_init: cannot register syscall\n");
	mtx_init(&rpctls_connect_lock, "rpctls_connect_lock", NULL,
	    MTX_DEF);
	mtx_init(&rpctls_server_lock, "rpctls_server_lock", NULL,
	    MTX_DEF);
	rpctls_null_verf.oa_flavor = AUTH_NULL;
	rpctls_null_verf.oa_base = RPCTLS_START_STRING;
	rpctls_null_verf.oa_length = strlen(RPCTLS_START_STRING);
}
SYSINIT(rpctls_init, SI_SUB_KMEM, SI_ORDER_ANY, rpctls_init, NULL);

int
sys_gssd_syscall(struct thread *td, struct gssd_syscall_args *uap)
{
        struct sockaddr_un sun;
        struct netconfig *nconf;
	struct file *fp;
	struct socket *so;
	char path[MAXPATHLEN], *pathp;
	int fd = -1, error, retry_count = 5;
	CLIENT *cl, *oldcl;
	bool ssd;
        
printf("in gssd syscall\n");
	error = priv_check(td, PRIV_NFS_DAEMON);
printf("aft priv_check=%d\n", error);
	if (error != 0)
		return (error);

#ifdef notyet
	switch (uap->op) {
	case RPCTLS_SYSC_SETPATH:
#else
		error = copyinstr(uap->path, path, sizeof(path), NULL);
printf("setting err=%d path=%s\n", error, path);
	if (error != 0)
		return (error);
	if (path[0] == 'S') {
		ssd = true;
		pathp = &path[1];
	} else {
		ssd = false;
		pathp = &path[0];
	}
	if (pathp[0] == '/' || pathp[0] == '\0') {
#endif
	if (ssd) {
		if (error == 0 && strlen(pathp) + 1 > sizeof(sun.sun_path))
			error = EINVAL;
	
		if (error == 0 && pathp[0] != '\0') {
			sun.sun_family = AF_LOCAL;
			strlcpy(sun.sun_path, pathp, sizeof(sun.sun_path));
			sun.sun_len = SUN_LEN(&sun);
			
			nconf = getnetconfigent("local");
			cl = clnt_reconnect_create(nconf,
			    (struct sockaddr *)&sun, RPCTLSSD, RPCTLSSDVERS,
			    RPC_MAXDATASIZE, RPC_MAXDATASIZE);
printf("got cl=%p\n", cl);
			/*
			 * The number of retries defaults to INT_MAX, which
			 * effectively means an infinite, uninterruptable loop. 
			 * Limiting it to five retries keeps it from running
			 * forever.
			 */
			if (cl != NULL)
				CLNT_CONTROL(cl, CLSET_RETRIES, &retry_count);
		} else
			cl = NULL;
	
		mtx_lock(&rpctls_server_lock);
		oldcl = rpctls_server_handle;
		rpctls_server_handle = cl;
		mtx_unlock(&rpctls_server_lock);
	
printf("cl=%p oldcl=%p\n", cl, oldcl);
		if (oldcl != NULL) {
			CLNT_CLOSE(oldcl);
			CLNT_RELEASE(oldcl);
		}
	} else {
		if (error == 0 && strlen(pathp) + 1 > sizeof(sun.sun_path))
			error = EINVAL;
	
		if (error == 0 && pathp[0] != '\0') {
			sun.sun_family = AF_LOCAL;
			strlcpy(sun.sun_path, pathp, sizeof(sun.sun_path));
			sun.sun_len = SUN_LEN(&sun);
			
			nconf = getnetconfigent("local");
			cl = clnt_reconnect_create(nconf,
			    (struct sockaddr *)&sun, RPCTLSCD, RPCTLSCDVERS,
			    RPC_MAXDATASIZE, RPC_MAXDATASIZE);
printf("got cl=%p\n", cl);
			/*
			 * The number of retries defaults to INT_MAX, which
			 * effectively means an infinite, uninterruptable loop. 
			 * Limiting it to five retries keeps it from running
			 * forever.
			 */
			if (cl != NULL)
				CLNT_CONTROL(cl, CLSET_RETRIES, &retry_count);
		} else
			cl = NULL;
	
		mtx_lock(&rpctls_connect_lock);
		oldcl = rpctls_connect_handle;
		rpctls_connect_handle = cl;
		mtx_unlock(&rpctls_connect_lock);
	
printf("cl=%p oldcl=%p\n", cl, oldcl);
		if (oldcl != NULL) {
			CLNT_CLOSE(oldcl);
			CLNT_RELEASE(oldcl);
		}
	}
	} else if (path[0] == 'C') {
printf("In connect\n");
		error = EINVAL;
#ifdef KERN_TLS
		if (PMAP_HAS_DMAP != 0)
			error = 0;
#endif
		if (error == 0)
			error = falloc(td, &fp, &fd, 0);
		if (error == 0) {
printf("falloc=%d fd=%d\n", error, fd);
			mtx_lock(&rpctls_connect_lock);
			so = rpctls_connect_so;
			rpctls_connect_so = NULL;
			mtx_unlock(&rpctls_connect_lock);
			finit(fp, FREAD | FWRITE, DTYPE_SOCKET, so, &socketops);
			td->td_retval[0] = fd;
		}
printf("returning=%d\n", fd);
	} else if (path[0] == 'E') {
printf("In srvconnect\n");
		error = EINVAL;
#ifdef KERN_TLS
		if (PMAP_HAS_DMAP != 0)
			error = 0;
#endif
		if (error == 0)
			error = falloc(td, &fp, &fd, 0);
		if (error == 0) {
printf("srv falloc=%d fd=%d\n", error, fd);
			mtx_lock(&rpctls_server_lock);
			so = rpctls_server_so;
			rpctls_server_so = NULL;
			mtx_unlock(&rpctls_server_lock);
			finit(fp, FREAD | FWRITE, DTYPE_SOCKET, so, &socketops);
			td->td_retval[0] = fd;
		}
printf("srv returning=%d\n", fd);
	} else if (path[0] == 'F') {
printf("In EOserver\n");
		fd = strtol(&path[1], NULL, 10);
printf("srv fd=%d\n", fd);
		if (fd >= 0) {
			error = kern_close(td, fd);
printf("srv aft kern_close=%d\n", error);
		} else {
			printf("rpctlss fd negative\n");
			error = EINVAL;
		}
	}

	return (error);
}

/*
 * Acquire the rpctls_connect_handle and return it with a reference count,
 * if it is available.
 */
static CLIENT *
rpctls_connect_client(void)
{
	CLIENT *cl;

	mtx_lock(&rpctls_connect_lock);
	cl = rpctls_connect_handle;
	if (cl != NULL)
		CLNT_ACQUIRE(cl);
	mtx_unlock(&rpctls_connect_lock);
	return (cl);
}

/*
 * Acquire the rpctls_server_handle and return it with a reference count,
 * if it is available.
 */
static CLIENT *
rpctls_server_client(void)
{
	CLIENT *cl;

	mtx_lock(&rpctls_server_lock);
	cl = rpctls_server_handle;
	if (cl != NULL)
		CLNT_ACQUIRE(cl);
	mtx_unlock(&rpctls_server_lock);
	return (cl);
}

/* Do an upcall for a new socket connect using TLS. */
enum clnt_stat
rpctls_connect(CLIENT *newclient, struct socket *so, uint64_t *sslp)
{
	struct rpctlscd_connect_res res;
	struct rpc_callextra ext;
	struct timeval utimeout;
	enum clnt_stat stat;
	CLIENT *cl;
	int val;
	static bool rpctls_connect_busy = false;

printf("In rpctls_connect\n");
	cl = rpctls_connect_client();
printf("connect_client=%p\n", cl);
	if (cl == NULL)
		return (RPC_AUTHERROR);

	/* First, do the AUTH_TLS NULL RPC. */
	memset(&ext, 0, sizeof(ext));
	utimeout.tv_sec = 30;
	utimeout.tv_usec = 0;
	ext.rc_auth = authtls_create();
printf("authtls=%p\n", ext.rc_auth);
	stat = clnt_call_private(newclient, &ext, NULLPROC, (xdrproc_t)xdr_void,
	    NULL, (xdrproc_t)xdr_void, NULL, utimeout);
printf("aft NULLRPC=%d\n", stat);
	AUTH_DESTROY(ext.rc_auth);
	if (stat == RPC_AUTHERROR)
		return (stat);
	if (stat != RPC_SUCCESS)
		return (RPC_SYSTEMERROR);

	/* Serialize the connect upcalls. */
	mtx_lock(&rpctls_connect_lock);
	while (rpctls_connect_busy)
		msleep(&rpctls_connect_busy, &rpctls_connect_lock, PVFS,
		    "rtlscn", 0);
	rpctls_connect_busy = true;
	rpctls_connect_so = so;
	mtx_unlock(&rpctls_connect_lock);
printf("rpctls_conect so=%p\n", so);

	/* Temporarily block reception during the handshake upcall. */
	val = 1;
	CLNT_CONTROL(newclient, CLSET_BLOCKRCV, &val);

	/* Do the connect handshake upcall. */
	stat = rpctlscd_connect_1(NULL, &res, cl);
printf("aft connect upcall=%d\n", stat);
	if (stat == RPC_SUCCESS) {
		*sslp++ = res.sec;
		*sslp++ = res.usec;
		*sslp = res.ssl;
	}
	CLNT_RELEASE(cl);

	/* Unblock reception. */
	val = 0;
	CLNT_CONTROL(newclient, CLSET_BLOCKRCV, &val);

	/* Once the upcall is done, the daemon is done with the fp and so. */
	mtx_lock(&rpctls_connect_lock);
	rpctls_connect_so = NULL;
	rpctls_connect_busy = false;
	wakeup(&rpctls_connect_busy);
	mtx_unlock(&rpctls_connect_lock);
printf("aft wakeup\n");

	return (stat);
}

/* Do an upcall to shut down a socket using TLS. */
enum clnt_stat
rpctls_cl_disconnect(uint64_t sec, uint64_t usec, uint64_t ssl)
{
	struct rpctlscd_disconnect_arg arg;
	enum clnt_stat stat;
	CLIENT *cl;

printf("In rpctls_cl_disconnect\n");
	cl = rpctls_connect_client();
printf("disconnect_client=%p\n", cl);
	if (cl == NULL)
		return (RPC_FAILED);

	/* Do the disconnect upcall. */
	arg.sec = sec;
	arg.usec = usec;
	arg.ssl = ssl;
	stat = rpctlscd_disconnect_1(&arg, NULL, cl);
printf("aft disconnect upcall=%d\n", stat);
	CLNT_RELEASE(cl);
	return (stat);
}

enum clnt_stat
rpctls_srv_disconnect(uint64_t sec, uint64_t usec, uint64_t ssl)
{
	struct rpctlssd_disconnect_arg arg;
	enum clnt_stat stat;
	CLIENT *cl;

printf("In rpctls_srv_disconnect\n");
	cl = rpctls_server_client();
printf("srv disconnect_client=%p\n", cl);
	if (cl == NULL)
		return (RPC_FAILED);

	/* Do the disconnect upcall. */
	arg.sec = sec;
	arg.usec = usec;
	arg.ssl = ssl;
	stat = rpctlssd_disconnect_1(&arg, NULL, cl);
printf("aft srv disconnect upcall=%d\n", stat);
	CLNT_RELEASE(cl);
	return (stat);
}

/* Do an upcall for a new server socket using TLS. */
static enum clnt_stat
rpctls_server(struct socket *so, uint32_t *flags, uint64_t *sslp,
    uid_t *uid, int *ngrps, gid_t **gids)
{
	enum clnt_stat stat;
	CLIENT *cl;
	struct rpctlssd_connect_res res;
	gid_t *gidp;
	uint32_t *gidv;
	int i;
	static bool rpctls_server_busy = false;

printf("In rpctls_server\n");
	cl = rpctls_server_client();
printf("server_client=%p\n", cl);
	if (cl == NULL)
		return (RPC_SYSTEMERROR);

	/* Serialize the server upcalls. */
	mtx_lock(&rpctls_server_lock);
	while (rpctls_server_busy)
		msleep(&rpctls_server_busy, &rpctls_server_lock, PVFS,
		    "rtlssn", 0);
	rpctls_server_busy = true;
	rpctls_server_so = so;
	mtx_unlock(&rpctls_server_lock);
printf("rpctls_conect so=%p\n", so);

	/* Do the server upcall. */
	stat = rpctlssd_connect_1(NULL, &res, cl);
	if (stat == RPC_SUCCESS) {
		*flags = res.flags;
		*sslp++ = res.sec;
		*sslp++ = res.usec;
		*sslp = res.ssl;
		if ((*flags & (RPCTLS_FLAGS_CNUSER |
		    RPCTLS_FLAGS_DISABLED)) == RPCTLS_FLAGS_CNUSER) {
			*ngrps = res.gid.gid_len;
			*uid = res.uid;
			*gids = gidp = mem_alloc(*ngrps * sizeof(gid_t));
			gidv = res.gid.gid_val;
printf("got uid=%d ngrps=%d gidv=%p gids=%p\n", *uid, *ngrps, gidv, gids);
			for (i = 0; i < *ngrps; i++)
				*gidp++ = *gidv++;
		}
	}
printf("aft server upcall stat=%d flags=0x%x\n", stat, res.flags);
	CLNT_RELEASE(cl);

	/* Once the upcall is done, the daemon is done with the fp and so. */
	mtx_lock(&rpctls_server_lock);
	rpctls_server_so = NULL;
	rpctls_server_busy = false;
	wakeup(&rpctls_server_busy);
	mtx_unlock(&rpctls_server_lock);
printf("aft wakeup\n");

	return (stat);
}

/*
 * Handle the NULL RPC with authentication flavor of AUTH_TLS.
 * This is a STARTTLS command, so do the upcall to the rpctlssd daemon,
 * which will do the TLS handshake.
 */
enum auth_stat
_svcauth_rpcsec_tls(struct svc_req *rqst, struct rpc_msg *msg)

{
	bool_t call_stat;
	enum clnt_stat stat;
	SVCXPRT *xprt;
	uint32_t flags;
	uint64_t ssl[3];
	int ngrps;
	uid_t uid;
	gid_t *gidp;
	
	/* Initialize reply. */
	rqst->rq_verf = rpctls_null_verf;
printf("authtls: clen=%d vlen=%d fl=%d\n", rqst->rq_cred.oa_length, msg->rm_call.cb_verf.oa_length, msg->rm_call.cb_verf.oa_flavor);

	/* Check client credentials. */
	if (rqst->rq_cred.oa_length != 0 ||
	    msg->rm_call.cb_verf.oa_length != 0 ||
	    msg->rm_call.cb_verf.oa_flavor != AUTH_NULL)
		return (AUTH_BADCRED);
	
printf("authtls proc=%d\n", rqst->rq_proc);
	if (rqst->rq_proc != NULLPROC)
		return (AUTH_REJECTEDCRED);

	if (PMAP_HAS_DMAP == 0)
		return (AUTH_REJECTEDCRED);

#ifndef KERN_TLS
	return (AUTH_REJECTEDCRED);
#endif

	/*
	 * Disable reception for the krpc so that the TLS handshake can
	 * be done on the socket in the rpctlssd daemon.
	 */
	xprt = rqst->rq_xprt;
	sx_xlock(&xprt->xp_lock);
	xprt->xp_dontrcv = TRUE;
	sx_xunlock(&xprt->xp_lock);

	/*
	 * Send the reply to the NULL RPC with AUTH_TLS, which is the
	 * STARTTLS command for Sun RPC.
	 */
	call_stat = svc_sendreply(rqst, (xdrproc_t)xdr_void, NULL);
printf("authtls: null reply=%d\n", call_stat);
	if (!call_stat) {
		sx_xlock(&xprt->xp_lock);
		xprt->xp_dontrcv = FALSE;
		sx_xunlock(&xprt->xp_lock);
		xprt_active(xprt);	/* Harmless if already active. */
		return (AUTH_REJECTEDCRED);
	}

	/* Do an upcall to do the TLS handshake. */
	stat = rpctls_server(rqst->rq_xprt->xp_socket, &flags,
	    ssl, &uid, &ngrps, &gidp);

	/* Re-enable reception on the socket within the krpc. */
	sx_xlock(&xprt->xp_lock);
	xprt->xp_dontrcv = FALSE;
	if (stat == RPC_SUCCESS) {
		xprt->xp_tls = flags;
		xprt->xp_sslsec = ssl[0];
		xprt->xp_sslusec = ssl[1];
		xprt->xp_sslrefno = ssl[2];
		if ((flags & (RPCTLS_FLAGS_CNUSER |
		    RPCTLS_FLAGS_DISABLED)) == RPCTLS_FLAGS_CNUSER) {
			xprt->xp_ngrps = ngrps;
			xprt->xp_uid = uid;
			xprt->xp_gidp = gidp;
printf("got uid=%d ngrps=%d gidp=%p\n", uid, ngrps, gidp);
		}
	}
	sx_xunlock(&xprt->xp_lock);
	xprt_active(xprt);		/* Harmless if already active. */
printf("authtls: aft handshake stat=%d\n", stat);

	return (RPCSEC_GSS_NODISPATCH);
}

