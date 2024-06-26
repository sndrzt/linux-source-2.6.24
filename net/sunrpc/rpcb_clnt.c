/*
 * In-kernel rpcbind client supporting versions 2, 3, and 4 of the rpcbind
 * protocol
 *
 * Based on RFC 1833: "Binding Protocols for ONC RPC Version 2" and
 * RFC 3530: "Network File System (NFS) version 4 Protocol"
 *
 * Original: Gilles Quillard, Bull Open Source, 2005 <gilles.quillard@bull.net>
 * Updated: Chuck Lever, Oracle Corporation, 2007 <chuck.lever@oracle.com>
 *
 * Descended from net/sunrpc/pmap_clnt.c,
 *  Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/module.h>

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/kernel.h>
#include <linux/errno.h>

#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/sched.h>
#include <linux/sunrpc/xprtsock.h>

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY	RPCDBG_BIND
#endif

#define RPCBIND_PROGRAM		(100000u)
#define RPCBIND_PORT		(111u)

enum {
	RPCBPROC_NULL,
	RPCBPROC_SET,
	RPCBPROC_UNSET,
	RPCBPROC_GETPORT,
	RPCBPROC_GETADDR = 3,		/* alias for GETPORT */
	RPCBPROC_DUMP,
	RPCBPROC_CALLIT,
	RPCBPROC_BCAST = 5,		/* alias for CALLIT */
	RPCBPROC_GETTIME,
	RPCBPROC_UADDR2TADDR,
	RPCBPROC_TADDR2UADDR,
	RPCBPROC_GETVERSADDR,
	RPCBPROC_INDIRECT,
	RPCBPROC_GETADDRLIST,
	RPCBPROC_GETSTAT,
};

#define RPCB_HIGHPROC_2		RPCBPROC_CALLIT
#define RPCB_HIGHPROC_3		RPCBPROC_TADDR2UADDR
#define RPCB_HIGHPROC_4		RPCBPROC_GETSTAT

/*
 * r_addr
 *
 * Quoting RFC 3530, section 2.2:
 *
 * For TCP over IPv4 and for UDP over IPv4, the format of r_addr is the
 * US-ASCII string:
 *
 *	h1.h2.h3.h4.p1.p2
 *
 * The prefix, "h1.h2.h3.h4", is the standard textual form for
 * representing an IPv4 address, which is always four octets long.
 * Assuming big-endian ordering, h1, h2, h3, and h4, are respectively,
 * the first through fourth octets each converted to ASCII-decimal.
 * Assuming big-endian ordering, p1 and p2 are, respectively, the first
 * and second octets each converted to ASCII-decimal.  For example, if a
 * host, in big-endian order, has an address of 0x0A010307 and there is
 * a service listening on, in big endian order, port 0x020F (decimal
 * 527), then the complete universal address is "10.1.3.7.2.15".
 *
 * ...
 *
 * For TCP over IPv6 and for UDP over IPv6, the format of r_addr is the
 * US-ASCII string:
 *
 *	x1:x2:x3:x4:x5:x6:x7:x8.p1.p2
 *
 * The suffix "p1.p2" is the service port, and is computed the same way
 * as with universal addresses for TCP and UDP over IPv4.  The prefix,
 * "x1:x2:x3:x4:x5:x6:x7:x8", is the standard textual form for
 * representing an IPv6 address as defined in Section 2.2 of [RFC2373].
 * Additionally, the two alternative forms specified in Section 2.2 of
 * [RFC2373] are also acceptable.
 *
 * XXX: Currently this implementation does not explicitly convert the
 *      stored address to US-ASCII on non-ASCII systems.
 */
#define RPCB_MAXADDRLEN		(128u)

/*
 * r_owner
 *
 * The "owner" is allowed to unset a service in the rpcbind database.
 * We always use the following (arbitrary) fixed string.
 */
#define RPCB_OWNER_STRING	"rpcb"
#define RPCB_MAXOWNERLEN	sizeof(RPCB_OWNER_STRING)

static void			rpcb_getport_done(struct rpc_task *, void *);
static struct rpc_program	rpcb_program;

struct rpcbind_args {
	struct rpc_xprt *	r_xprt;

	u32			r_prog;
	u32			r_vers;
	u32			r_prot;
	unsigned short		r_port;
	const char *		r_netid;
	const char *		r_addr;
	const char *		r_owner;
};

static struct rpc_procinfo rpcb_procedures2[];
static struct rpc_procinfo rpcb_procedures3[];

struct rpcb_info {
	int			rpc_vers;
	struct rpc_procinfo *	rpc_proc;
};

static struct rpcb_info rpcb_next_version[];
static struct rpcb_info rpcb_next_version6[];

static void rpcb_getport_prepare(struct rpc_task *task, void *calldata)
{
	struct rpcbind_args *map = calldata;
	struct rpc_xprt *xprt = map->r_xprt;
	struct rpc_message msg = {
		.rpc_proc	= rpcb_next_version[xprt->bind_index].rpc_proc,
		.rpc_argp	= map,
		.rpc_resp	= &map->r_port,
	};

	rpc_call_setup(task, &msg, 0);
}

static void rpcb_map_release(void *data)
{
	struct rpcbind_args *map = data;

	xprt_put(map->r_xprt);
	kfree(map);
}

static const struct rpc_call_ops rpcb_getport_ops = {
	.rpc_call_prepare	= rpcb_getport_prepare,
	.rpc_call_done		= rpcb_getport_done,
	.rpc_release		= rpcb_map_release,
};

static void rpcb_wake_rpcbind_waiters(struct rpc_xprt *xprt, int status)
{
	xprt_clear_binding(xprt);
	rpc_wake_up_status(&xprt->binding, status);
}

static struct rpc_clnt *rpcb_create(char *hostname, struct sockaddr *srvaddr,
					int proto, int version, int privileged)
{
	struct rpc_create_args args = {
		.protocol	= proto,
		.address	= srvaddr,
		.addrsize	= sizeof(struct sockaddr_in),
		.servername	= hostname,
		.program	= &rpcb_program,
		.version	= version,
		.authflavor	= RPC_AUTH_UNIX,
		.flags		= (RPC_CLNT_CREATE_NOPING |
				   RPC_CLNT_CREATE_INTR),
	};

	switch (srvaddr->sa_family) {
	case AF_INET:
		((struct sockaddr_in *)srvaddr)->sin_port = htons(RPCBIND_PORT);
		break;
	case AF_INET6:
		((struct sockaddr_in6 *)srvaddr)->sin6_port = htons(RPCBIND_PORT);
		break;
	default:
		return NULL;
	}

	if (!privileged)
		args.flags |= RPC_CLNT_CREATE_NONPRIVPORT;
	return rpc_create(&args);
}

/**
 * rpcb_register - set or unset a port registration with the local rpcbind svc
 * @prog: RPC program number to bind
 * @vers: RPC version number to bind
 * @prot: transport protocol to use to make this request
 * @port: port value to register
 * @okay: result code
 *
 * port == 0 means unregister, port != 0 means register.
 *
 * This routine supports only rpcbind version 2.
 */
int rpcb_register(u32 prog, u32 vers, int prot, unsigned short port, int *okay)
{
	struct sockaddr_in sin = {
		.sin_family		= AF_INET,
		.sin_addr.s_addr	= htonl(INADDR_LOOPBACK),
	};
	struct rpcbind_args map = {
		.r_prog		= prog,
		.r_vers		= vers,
		.r_prot		= prot,
		.r_port		= port,
	};
	struct rpc_message msg = {
		.rpc_proc	= &rpcb_procedures2[port ?
					RPCBPROC_SET : RPCBPROC_UNSET],
		.rpc_argp	= &map,
		.rpc_resp	= okay,
	};
	struct rpc_clnt *rpcb_clnt;
	int error = 0;

	dprintk("RPC:       %sregistering (%u, %u, %d, %u) with local "
			"rpcbind\n", (port ? "" : "un"),
			prog, vers, prot, port);

	rpcb_clnt = rpcb_create("localhost", (struct sockaddr *) &sin,
					XPRT_TRANSPORT_UDP, 2, 1);
	if (IS_ERR(rpcb_clnt))
		return PTR_ERR(rpcb_clnt);

	error = rpc_call_sync(rpcb_clnt, &msg, 0);

	rpc_shutdown_client(rpcb_clnt);
	if (error < 0)
		printk(KERN_WARNING "RPC: failed to contact local rpcbind "
				"server (errno %d).\n", -error);
	dprintk("RPC:       registration status %d/%d\n", error, *okay);

	return error;
}

/**
 * rpcb_getport_sync - obtain the port for an RPC service on a given host
 * @sin: address of remote peer
 * @prog: RPC program number to bind
 * @vers: RPC version number to bind
 * @prot: transport protocol to use to make this request
 *
 * Called from outside the RPC client in a synchronous task context.
 * Uses default timeout parameters specified by underlying transport.
 *
 * XXX: Needs to support IPv6, and rpcbind versions 3 and 4
 */
int rpcb_getport_sync(struct sockaddr_in *sin, __u32 prog,
		      __u32 vers, int prot)
{
	struct rpcbind_args map = {
		.r_prog		= prog,
		.r_vers		= vers,
		.r_prot		= prot,
		.r_port		= 0,
	};
	struct rpc_message msg = {
		.rpc_proc	= &rpcb_procedures2[RPCBPROC_GETPORT],
		.rpc_argp	= &map,
		.rpc_resp	= &map.r_port,
	};
	struct rpc_clnt	*rpcb_clnt;
	char hostname[40];
	int status;

	dprintk("RPC:       %s(" NIPQUAD_FMT ", %u, %u, %d)\n",
		__FUNCTION__, NIPQUAD(sin->sin_addr.s_addr), prog, vers, prot);

	sprintf(hostname, NIPQUAD_FMT, NIPQUAD(sin->sin_addr.s_addr));
	rpcb_clnt = rpcb_create(hostname, (struct sockaddr *)sin, prot, 2, 0);
	if (IS_ERR(rpcb_clnt))
		return PTR_ERR(rpcb_clnt);

	status = rpc_call_sync(rpcb_clnt, &msg, 0);
	rpc_shutdown_client(rpcb_clnt);

	if (status >= 0) {
		if (map.r_port != 0)
			return map.r_port;
		status = -EACCES;
	}
	return status;
}
EXPORT_SYMBOL_GPL(rpcb_getport_sync);

/*
 * In the case where rpc clients have been cloned, we want to make
 * sure that we use the program number/version etc of the actual
 * owner of the xprt. To do so, we walk back up the tree of parents
 * to find whoever created the transport and/or whoever has the
 * autobind flag set.
 */
static struct rpc_clnt *rpcb_find_transport_owner(struct rpc_clnt *clnt)
{
	struct rpc_clnt *parent = clnt->cl_parent;

	while (parent != clnt) {
		if (parent->cl_xprt != clnt->cl_xprt)
			break;
		if (clnt->cl_autobind)
			break;
		clnt = parent;
		parent = parent->cl_parent;
	}
	return clnt;
}

/**
 * rpcb_getport_async - obtain the port for a given RPC service on a given host
 * @task: task that is waiting for portmapper request
 *
 * This one can be called for an ongoing RPC request, and can be used in
 * an async (rpciod) context.
 */
void rpcb_getport_async(struct rpc_task *task)
{
	struct rpc_clnt *clnt;
	int bind_version;
	struct rpc_xprt *xprt;
	struct rpc_clnt	*rpcb_clnt;
	static struct rpcbind_args *map;
	struct rpc_task	*child;
	struct sockaddr addr;
	int status;
	struct rpcb_info *info;

	clnt = rpcb_find_transport_owner(task->tk_client);
	xprt = clnt->cl_xprt;

	dprintk("RPC: %5u %s(%s, %u, %u, %d)\n",
		task->tk_pid, __FUNCTION__,
		clnt->cl_server, clnt->cl_prog, clnt->cl_vers, xprt->prot);

	if (xprt_test_and_set_binding(xprt)) {
		status = -EAGAIN;	/* tell caller to check again */
		dprintk("RPC: %5u %s: waiting for another binder\n",
			task->tk_pid, __FUNCTION__);
		goto bailout_nowake;
	}

	/* Put self on queue before sending rpcbind request, in case
	 * rpcb_getport_done completes before we return from rpc_run_task */
	rpc_sleep_on(&xprt->binding, task, NULL, NULL);

	/* Someone else may have bound if we slept */
	if (xprt_bound(xprt)) {
		status = 0;
		dprintk("RPC: %5u %s: already bound\n",
			task->tk_pid, __FUNCTION__);
		goto bailout_nofree;
	}

	rpc_peeraddr(clnt, (void *)&addr, sizeof(addr));

	/* Don't ever use rpcbind v2 for AF_INET6 requests */
	switch (addr.sa_family) {
	case AF_INET:
		info = rpcb_next_version;
		break;
	case AF_INET6:
		info = rpcb_next_version6;
		break;
	default:
		status = -EAFNOSUPPORT;
		dprintk("RPC: %5u %s: bad address family\n",
				task->tk_pid, __FUNCTION__);
		goto bailout_nofree;
	}
	if (info[xprt->bind_index].rpc_proc == NULL) {
		xprt->bind_index = 0;
		status = -EPFNOSUPPORT;
		dprintk("RPC: %5u %s: no more getport versions available\n",
			task->tk_pid, __FUNCTION__);
		goto bailout_nofree;
	}
	bind_version = info[xprt->bind_index].rpc_vers;

	dprintk("RPC: %5u %s: trying rpcbind version %u\n",
		task->tk_pid, __FUNCTION__, bind_version);

	rpcb_clnt = rpcb_create(clnt->cl_server, &addr, xprt->prot,
				bind_version, 0);
	if (IS_ERR(rpcb_clnt)) {
		status = PTR_ERR(rpcb_clnt);
		dprintk("RPC: %5u %s: rpcb_create failed, error %ld\n",
			task->tk_pid, __FUNCTION__, PTR_ERR(rpcb_clnt));
		goto bailout_nofree;
	}

	map = kzalloc(sizeof(struct rpcbind_args), GFP_ATOMIC);
	if (!map) {
		status = -ENOMEM;
		dprintk("RPC: %5u %s: no memory available\n",
			task->tk_pid, __FUNCTION__);
		goto bailout_nofree;
	}
	map->r_prog = clnt->cl_prog;
	map->r_vers = clnt->cl_vers;
	map->r_prot = xprt->prot;
	map->r_port = 0;
	map->r_xprt = xprt_get(xprt);
	map->r_netid = rpc_peeraddr2str(clnt, RPC_DISPLAY_NETID);
	map->r_addr = rpc_peeraddr2str(rpcb_clnt, RPC_DISPLAY_UNIVERSAL_ADDR);
	map->r_owner = RPCB_OWNER_STRING;	/* ignored for GETADDR */

	child = rpc_run_task(rpcb_clnt, RPC_TASK_ASYNC, &rpcb_getport_ops, map);
	rpc_release_client(rpcb_clnt);
	if (IS_ERR(child)) {
		status = -EIO;
		dprintk("RPC: %5u %s: rpc_run_task failed\n",
			task->tk_pid, __FUNCTION__);
		goto bailout;
	}

	xprt->stat.bind_count++;
	rpc_put_task(child);
	return;

bailout:
	kfree(map);
	xprt_put(xprt);
bailout_nofree:
	rpcb_wake_rpcbind_waiters(xprt, status);
bailout_nowake:
	task->tk_status = status;
}
EXPORT_SYMBOL_GPL(rpcb_getport_async);

/*
 * Rpcbind child task calls this callback via tk_exit.
 */
static void rpcb_getport_done(struct rpc_task *child, void *data)
{
	struct rpcbind_args *map = data;
	struct rpc_xprt *xprt = map->r_xprt;
	int status = child->tk_status;

	/* Garbage reply: retry with a lesser rpcbind version */
	if (status == -EIO)
		status = -EPROTONOSUPPORT;

	/* rpcbind server doesn't support this rpcbind protocol version */
	if (status == -EPROTONOSUPPORT)
		xprt->bind_index++;

	if (status < 0) {
		/* rpcbind server not available on remote host? */
		xprt->ops->set_port(xprt, 0);
	} else if (map->r_port == 0) {
		/* Requested RPC service wasn't registered on remote host */
		xprt->ops->set_port(xprt, 0);
		status = -EACCES;
	} else {
		/* Succeeded */
		xprt->ops->set_port(xprt, map->r_port);
		xprt_set_bound(xprt);
		status = 0;
	}

	dprintk("RPC: %5u rpcb_getport_done(status %d, port %u)\n",
			child->tk_pid, status, map->r_port);

	rpcb_wake_rpcbind_waiters(xprt, status);
}

static int rpcb_encode_mapping(struct rpc_rqst *req, __be32 *p,
			       struct rpcbind_args *rpcb)
{
	dprintk("RPC:       rpcb_encode_mapping(%u, %u, %d, %u)\n",
			rpcb->r_prog, rpcb->r_vers, rpcb->r_prot, rpcb->r_port);
	*p++ = htonl(rpcb->r_prog);
	*p++ = htonl(rpcb->r_vers);
	*p++ = htonl(rpcb->r_prot);
	*p++ = htonl(rpcb->r_port);

	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int rpcb_decode_getport(struct rpc_rqst *req, __be32 *p,
			       unsigned short *portp)
{
	*portp = (unsigned short) ntohl(*p++);
	dprintk("RPC:      rpcb_decode_getport result %u\n",
			*portp);
	return 0;
}

static int rpcb_decode_set(struct rpc_rqst *req, __be32 *p,
			   unsigned int *boolp)
{
	*boolp = (unsigned int) ntohl(*p++);
	dprintk("RPC:      rpcb_decode_set result %u\n",
			*boolp);
	return 0;
}

static int rpcb_encode_getaddr(struct rpc_rqst *req, __be32 *p,
			       struct rpcbind_args *rpcb)
{
	dprintk("RPC:       rpcb_encode_getaddr(%u, %u, %s)\n",
			rpcb->r_prog, rpcb->r_vers, rpcb->r_addr);
	*p++ = htonl(rpcb->r_prog);
	*p++ = htonl(rpcb->r_vers);

	p = xdr_encode_string(p, rpcb->r_netid);
	p = xdr_encode_string(p, rpcb->r_addr);
	p = xdr_encode_string(p, rpcb->r_owner);

	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	return 0;
}

static int rpcb_decode_getaddr(struct rpc_rqst *req, __be32 *p,
			       unsigned short *portp)
{
	char *addr;
	u32 addr_len;
	int c, i, f, first, val;

	*portp = 0;
	addr_len = ntohl(*p++);

	/*
	 * Simple sanity check.  The smallest possible universal
	 * address is an IPv4 address string containing 11 bytes.
	 */
	if (addr_len < 11 || addr_len > RPCB_MAXADDRLEN)
		goto out_err;

	/*
	 * Start at the end and walk backwards until the first dot
	 * is encountered.  When the second dot is found, we have
	 * both parts of the port number.
	 */
	addr = (char *)p;
	val = 0;
	first = 1;
	f = 1;
	for (i = addr_len - 1; i > 0; i--) {
		c = addr[i];
		if (c >= '0' && c <= '9') {
			val += (c - '0') * f;
			f *= 10;
		} else if (c == '.') {
			if (first) {
				*portp = val;
				val = first = 0;
				f = 1;
			} else {
				*portp |= (val << 8);
				break;
			}
		}
	}

	/*
	 * Simple sanity check.  If we never saw a dot in the reply,
	 * then this was probably just garbage.
	 */
	if (first)
		goto out_err;

	dprintk("RPC:       rpcb_decode_getaddr port=%u\n", *portp);
	return 0;

out_err:
	dprintk("RPC:       rpcbind server returned malformed reply\n");
	return -EIO;
}

#define RPCB_program_sz		(1u)
#define RPCB_version_sz		(1u)
#define RPCB_protocol_sz	(1u)
#define RPCB_port_sz		(1u)
#define RPCB_boolean_sz		(1u)

#define RPCB_netid_sz		(1+XDR_QUADLEN(RPCBIND_MAXNETIDLEN))
#define RPCB_addr_sz		(1+XDR_QUADLEN(RPCB_MAXADDRLEN))
#define RPCB_ownerstring_sz	(1+XDR_QUADLEN(RPCB_MAXOWNERLEN))

#define RPCB_mappingargs_sz	RPCB_program_sz+RPCB_version_sz+	\
				RPCB_protocol_sz+RPCB_port_sz
#define RPCB_getaddrargs_sz	RPCB_program_sz+RPCB_version_sz+	\
				RPCB_netid_sz+RPCB_addr_sz+		\
				RPCB_ownerstring_sz

#define RPCB_setres_sz		RPCB_boolean_sz
#define RPCB_getportres_sz	RPCB_port_sz

/*
 * Note that RFC 1833 does not put any size restrictions on the
 * address string returned by the remote rpcbind database.
 */
#define RPCB_getaddrres_sz	RPCB_addr_sz

#define PROC(proc, argtype, restype)					\
	[RPCBPROC_##proc] = {						\
		.p_proc		= RPCBPROC_##proc,			\
		.p_encode	= (kxdrproc_t) rpcb_encode_##argtype,	\
		.p_decode	= (kxdrproc_t) rpcb_decode_##restype,	\
		.p_arglen	= RPCB_##argtype##args_sz,		\
		.p_replen	= RPCB_##restype##res_sz,		\
		.p_statidx	= RPCBPROC_##proc,			\
		.p_timer	= 0,					\
		.p_name		= #proc,				\
	}

/*
 * Not all rpcbind procedures described in RFC 1833 are implemented
 * since the Linux kernel RPC code requires only these.
 */
static struct rpc_procinfo rpcb_procedures2[] = {
	PROC(SET,		mapping,	set),
	PROC(UNSET,		mapping,	set),
	PROC(GETADDR,		mapping,	getport),
};

static struct rpc_procinfo rpcb_procedures3[] = {
	PROC(SET,		mapping,	set),
	PROC(UNSET,		mapping,	set),
	PROC(GETADDR,		getaddr,	getaddr),
};

static struct rpc_procinfo rpcb_procedures4[] = {
	PROC(SET,		mapping,	set),
	PROC(UNSET,		mapping,	set),
	PROC(GETVERSADDR,	getaddr,	getaddr),
};

static struct rpcb_info rpcb_next_version[] = {
#ifdef CONFIG_SUNRPC_BIND34
	{ 4, &rpcb_procedures4[RPCBPROC_GETVERSADDR] },
	{ 3, &rpcb_procedures3[RPCBPROC_GETADDR] },
#endif
	{ 2, &rpcb_procedures2[RPCBPROC_GETPORT] },
	{ 0, NULL },
};

static struct rpcb_info rpcb_next_version6[] = {
#ifdef CONFIG_SUNRPC_BIND34
	{ 4, &rpcb_procedures4[RPCBPROC_GETVERSADDR] },
	{ 3, &rpcb_procedures3[RPCBPROC_GETADDR] },
#endif
	{ 0, NULL },
};

static struct rpc_version rpcb_version2 = {
	.number		= 2,
	.nrprocs	= RPCB_HIGHPROC_2,
	.procs		= rpcb_procedures2
};

static struct rpc_version rpcb_version3 = {
	.number		= 3,
	.nrprocs	= RPCB_HIGHPROC_3,
	.procs		= rpcb_procedures3
};

static struct rpc_version rpcb_version4 = {
	.number		= 4,
	.nrprocs	= RPCB_HIGHPROC_4,
	.procs		= rpcb_procedures4
};

static struct rpc_version *rpcb_version[] = {
	NULL,
	NULL,
	&rpcb_version2,
	&rpcb_version3,
	&rpcb_version4
};

static struct rpc_stat rpcb_stats;

static struct rpc_program rpcb_program = {
	.name		= "rpcbind",
	.number		= RPCBIND_PROGRAM,
	.nrvers		= ARRAY_SIZE(rpcb_version),
	.version	= rpcb_version,
	.stats		= &rpcb_stats,
};
