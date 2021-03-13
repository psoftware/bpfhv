#define _GNU_SOURCE
#include "tsc.h"
#include "sched16.h"
#include "cqueue.h"

#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>

size_t cqueue_stat_size = 10000000;

struct cqueue *
cqueue_create(uint32_t user_size, const char *fmt, ...)
{
    uint32_t real_size;
    struct cqueue *q;
    va_list ap;

    /*
     * The queue size in bytes must be a multiple of the cache line,
     * so we force that the number of slots is
     * also a multiple of QCACHE_NUM.
     * Further, for efficiency we need a cache line to hold an
     * integer number of entries. This is a compile time constraint.
     */
    real_size = (user_size + QCACHE_NUM - 1) & ~(QCACHE_NUM - 1);

    q = SAFE_CALLOC(sizeof(struct cqueue) + sizeof(struct my_pkt)*real_size);

    va_start(ap, fmt);
    vsnprintf(q->name, CQUEUE_MAXNAME, fmt, ap); 
    va_end(ap);

    q->user_size = user_size;
    q->real_size = real_size;

#ifdef STATS
    q->stats = SAFE_CALLOC(sizeof(struct pkt_stat) * cqueue_stat_size);
    q->stat_size = cqueue_stat_size;
#endif

    return q;
}

/*
 * sleep until the scheduler has started and filled the p_s_head field
 */
void
cqueue_wait_ready(struct cqueue *q)
{
    while (ACCESS_ONCE(q->p_s_head) == NULL) {
	ND(1, "waiting for queue %p to be ready", q);
	usleep(1000);
    }
}

void
cqueue_cli_init(struct cqueue_client *cq, struct cqueue *q)
{
    bzero(cq, sizeof(*cq));
    cq->q = q;
}
	
/* client: insert a new packet in the queue (drop if full) */
void
cqueue_cli_put(struct cqueue_client *q, uint64_t buf, uint16_t len)
{
    if (unlikely(++q->n_pending >= q->q->user_size)) {
        /* queue is full, let's drop */
        q->n_pending--;
	q->n_cli_drp++;
	return;
    }

    struct my_pkt *p = &q->q->queue[q->c_tail];
    p->bufptr = buf;
    p->len = len;
    q->st[q->c_tail].cli_enq = rdtsc(); /* record the timestamp */
    q->c_tail = cqueue_next(q->q, q->c_tail);
#if 0
#if defined(STATS) || !defined(NDEBUG)
    m->seq = q->n_cli_put;
#endif
#ifdef STATS
    if (m->seq < q->stat_size) {
	struct pkt_stat *s = q->stats + m->seq;
	bzero(s, sizeof(*s));
	s->cli_enq = rdtsc();
	s->pkt_len = len;
	q->stat_last_seq = m->seq;
    }
#endif
#endif
    q->n_cli_put++;
    q->n_cli_put_bytes += len;
    ND("pkt %u %u", c_tail, len);
}


/* client: extract the packets marked by the scheduler */
uint16_t
cqueue_cli_get(struct cqueue_client *q, uint32_t max)
{
    uint16_t rv = 0;
    /* We use a locally cached copy of the scheduler
     * head, to avoid excessive cacheline bouncing.
     * This function updates the cached copy and then
     * consumes all the packet we got.
     * NOTE the ACCESS_ONCE() seems to have no impact on performance.
     */
    if (q->c_shead == q->c_head) {
        q->c_shead = ACCESS_ONCE(*q->q->p_s_head);
    }

    while (q->c_head != q->c_shead && rv < max) {
	rv++;
	// q->q->queue[q->c_head].ts[1] = rdtsc();
	q->n_cli_lag += rdtsc() - q->st[q->c_head].cli_enq;
	struct my_pkt* p = &q->q->queue[q->c_head];
	p->bufptr = 0; /* TODO: this should be useless*/
	p->len = 0; /* mark as read */
	q->c_head = cqueue_next(q->q, q->c_head);
        q->n_pending--;
	q->n_cli_get++;
    }

    if (rv) {
        q->n_cli_slow_read++;
    }

    return rv;
}

/* client: returns 1 iff the client has no other work to do */
int
cqueue_cli_done(struct cqueue_client *q)
{
    return (ACCESS_ONCE(*q->q->p_s_head) == q->c_head);
}

void
cqueue_sched_init(struct cqueue_sched *cq, struct cqueue *q)
{
    bzero(cq, sizeof(*cq));
    cq->q = q;
}

/*
 * called by the scheduler to advance the pointer to marked packets.
 */
void
cqueue_mark(struct cqueue_sched *q, uint16_t npkts)
{
    ND("m %u", s_nhead);
    //assert(q->queue + q->s_nhead == m); // XXX should hold if q <=> flow_id
    /* s_head must point to the first unmarked mbuf.
     * Update the scheduler's copy, not the public one.
     */
    q->s_nhead += npkts;
    if (unlikely(q->s_nhead >= q->q->real_size))
	q->s_nhead -= q->q->real_size;
    q->n_sch_put += npkts;
#if 0
    if (m->seq < q->stat_size) {
	struct pkt_stat *s = q->stats + m->seq;
	s->sch_deq = rdtsc() - s->sch_enq;
    }
#endif
}


void
cqueue_show_counters(struct cqueue *q)
{
#if 0
    printf("%s:         %-10s %-10s %-10s\n", q->name, "drop", "put", "get");
    printf("  client    %10"PRIu64" %10"PRIu64" %10"PRIu64"\n",
	    q->n_cli_drp, q->n_cli_put, q->n_cli_get);
    printf("  scheduler %10"PRIu64" %10"PRIu64" %10"PRIu64"\n",
	    q->n_sch_drp, q->n_sch_put, q->n_sch_get);
#else
    (void) q;
#endif
}

void
cqueue_dump_stats(struct cqueue *q, FILE *f)
{
#ifdef STATS
    uint64_t i;

    for (i = 0; i <= q->stat_last_seq; i++) {
	struct pkt_stat *s = q->stats + i;
	fprintf(f, "%"PRIu64"\t%"PRIu32"\t%"PRIu32"\t%"PRIu32"\t%"PRIu32"\t%"PRIu16"\t%s\n",
	    s->cli_enq,
	    s->sch_enq,
	    s->sch_deq,
	    s->cli_deq,
	    s->cli_deq,
	    s->pkt_len,
	    (s->flags & PSF_DROPPED) ? "D" : "");
    } 
#else
    (void)q;
    (void)f;
#endif
}
