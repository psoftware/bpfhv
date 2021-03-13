/*
 * $FreeBSD$
 *
 * userspace compatibility code for dummynet schedulers
 */

#ifndef _DN_TEST_H
#define _DN_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#define _GNU_SOURCE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>	/* bzero, ffs, ... */
#include <string.h>	/* strcmp */
#include <errno.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

extern int debug;
#define ND(fmt, args...) do {} while (0)
#define D1(fmt, args...) do {} while (0)
#define D(fmt, args...) fprintf(stderr, "%-10s %4d %-8s " fmt "\n",      \
        __FILE__, __LINE__, __FUNCTION__, ## args)
#define DX(lev, fmt, args...) do {              \
        if (debug > lev) D(fmt, ## args); } while (0)


#ifndef offsetof /* used in mylist.h and wf2q+ */
#define offsetof(t,m) (int)(intptr_t)((&((t *)0L)->m))
#endif

#if defined(__APPLE__) // XXX osx
typedef unsigned int u_int;
#endif /* osx */

#include <mylist.h>

/* prevent include of other system headers */
#define	_NETINET_IP_VAR_H_	/* ip_fw_args */
#define _IPFW2_H
#define _SYS_MBUF_H_

#define unlikely(x)     __builtin_expect((x),0)

#include "sched16.h"

enum	{
	DN_QUEUE,
};

enum	{
	DN_SCHED_FIFO,
	DN_SCHED_WF2QP,
};

/* from ip_dummynet.h, fields used in ip_dn_private.h */
struct dn_id {
	uint16_t 	len; /* total len inc. this header */
	uint8_t		type;
	uint8_t		subtype;
//	uint32_t	id;	/* generic id */
};

/* (from ip_dummynet.h)
 * A flowset, which is a template for flows. Contains parameters
 * from the command line: id, target scheduler, queue sizes, plr,
 * flow masks, buckets for the flow hash, and possibly scheduler-
 * specific parameters (weight, quantum and so on).
 */
struct dn_fs {
        /* generic scheduler parameters. Leave them at -1 if unset.
         * Now we use 0: weight, 1: lmax, 2: priority
         */
	int par[4];	/* flowset parameters */

	/* simulation entries.
	 * 'index' is not strictly necessary
	 * y is used for the inverse mapping ,
	 */
	int index;
	int y;	/* inverse mapping */
	int base_y;	/* inverse mapping */
	int next_y;	/* inverse mapping */
	int n_flows;
	int first_flow;
	int next_flow;	/* first_flow + n_flows */
	/*
	 * when generating, let 'cur' go from 0 to n_flows-1,
	 * then point to flow first_flow + cur
	 */
	int	cur;
};

/* (ip_dummynet.h)
 * scheduler template, indicating name, number, mask and buckets
 */
struct dn_sch {
};

#if 0
struct mbuf {
        struct {
                int len;
        } m_pkthdr;
#ifndef MY_MQ_LEN
        struct mbuf *m_nextpkt;
#endif
        struct mbuf *m_freelist;	/* XXX testing */
	uint32_t flow_id;	/* for testing, index of a flow */
	//int flowset_id;	/* for testing, index of a flowset */
	//void *cfg;	/* config args */
};
#endif

/* (from ip_dummynet.h)
 * dn_flow collects flow_id and stats for queues and scheduler
 * instances, and is used to pass these info to userland.
 * oid.type/oid.subtype describe the object, oid.id is number
 * of the parent object.
 *
 */
struct dn_flow {
	struct dn_id oid;
	uint64_t tot_pkts;	/* updated on enqueue */
	uint64_t tot_bytes;	/* updated on enqueue */
	uint32_t length;	/* Queue length, in packets */
	uint32_t len_bytes;	/* Queue length, in bytes */
	uint32_t drops;
	//uint32_t flow_id;

	/*
	 * the following fields are used by the traffic generator.
	 */
	double q_wfi;		/* max of the wfi in this run */
	struct list_head h;	/* used by the generator */

	/* bytes served by the flow since the last backlog time */
	uint64_t bytes;		/* updated on dequeue */
	/* bytes served by the system at the last backlog time  */
	uint64_t sch_bytes;

	/*
	 * Each flow can have its own pool of mbufs, of size mbq_len.
	 * A buffer can be used if its length is zero. The length
	 * is set by the producer before enqueue, and reset by the
	 * consumer after dequeue.
	 */
	uint32_t	mbq_len;
	uint32_t	mbq_tail; /* next mbuf to transmit */
	struct mbuf *mb;	/* our private pool of mbufs */
};

/* the link */
struct dn_link {
};

struct ip_fw_args; /* used in prototype in ip_dn_private.h */
struct ipfw_flow_id { /* used in ip_dn_private.h */
};

#define MALLOC_DECLARE(x)	extern volatile int __dummy__ ## x
#define KASSERT(x, y)	do { if (!(x)) printf y ; exit(0); } while (0)

typedef void * module_t;

struct _md_t {
	const char *name;
	int (*f)(module_t, int, void *);
	void *p;
};

typedef struct _md_t moduledata_t;

#define DECLARE_MODULE(name, b, c, d)	\
	moduledata_t *_g_##name = & b
#define MODULE_DEPEND(a, b, c, d, e)

#include <dn_heap.h>
#include <ip_dn_private.h>
#include <dn_sched.h>

#ifndef __FreeBSD__
int fls(int);
#endif

static inline int
mq_append(struct mq *q, struct mbuf *m)
{
#ifdef MY_MQ_LEN /* XXX note this can fail! */
	uint32_t i = q->q_tail + 1;
	if (q->q_len ==0) {
		D("initialize queue %d h %d t %d", q->fid, q->q_head, q->q_tail);
		q->q_len = MY_MQ_LEN;
	}
	if (i == q->q_len)
		i = 0;
	if (i == q->q_head) { /* queue full */
		D("queue full");
		return 1;
	}
	q->q[q->q_tail] = m;
	if (q->q_tail == q->q_head) /* first element */
		q->head = m;
	q->q_tail = i;
	ND("q %d h %d t %d %s", q->fid, q->q_head, q->q_tail,
		(q->head == m) ? "FIRST" : "");
#else
        if (q->head == NULL)
                q->head = m;
        else
                q->tail->m_nextpkt = m;
        q->tail = m;
        m->m_nextpkt = NULL;
#endif
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _DN_TEST_H */
