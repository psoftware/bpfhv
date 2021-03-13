/*
 * BSD Copyright
 *
 * (C) 2016 Luigi Rizzo
 */

/*
 * main program for testing the scheduler

 Create one object for the actual scheduler, one for the VS
 Launch one thread per client

 */

#include "sched16.h"
#include "tsc.h"
#include <stdarg.h>
#include <inttypes.h>
#include <assert.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <semaphore.h>
#include <poll.h>
#ifdef linux
#include <sys/eventfd.h>
#else
#define eventfd(_a, _b) (-1)
#endif

#include <net/if.h>
#define NETMAP_WITH_LIBS
#include <libnetmap.h>

#define UDP_PKT_HLEN (14+20+8)

int debug; // XXX global for dn_sched_qfq

/* Prepare a dummy Ethernet broadcast frame. */
static void
packet_eth_init(uint8_t *buf)
{
    memset(buf, 0xff, 6);
    memset(buf + 6, 0x00, 6);
    buf[12] = 0x08;
    buf[13] = 0x00;
}

/*
 * print schedule
 */
static void
print_schedule(struct sched_all *f)
{
    uint32_t i, j;

    printf("# --- begin configuration ---------------------\n");
    printf("# using %d threads\n", f->n_threads);
    for (i = 0; i < f->n_threads; i++) {
        struct sched_td *cur = &f->td[i];

	printf("thread %2d # duration   bandwidth  pktsize    burstsize\n", i);
	for (j = 0; j < cur->wl_len; j++) {
	    struct _txp *t = &cur->worklist[j];
	    char *ty = (t->bpp == 1) ? "p" : "";

	    printf("           %10.3e %10.3e%s %10d %10d\n",
		t->duration, t->rate, ty, t->pkt_len, t->burst_len);
	}
    }
    printf("# --- end configuration -----------------------\n");
}

/*
 * load schedule from a file for all threads
 */
static void
load_schedule(struct sched_all *f, const char *cfile)
{
    FILE *fin;
#define BUFSIZE 256
    char buf[BUFSIZE];
    int ln;
    long thigh = 0, tid = 0; /* -1 means invalid */

#define warn(fmt, ...) fprintf(stderr, "%s:%d: "fmt"\n", cfile, ln, ##__VA_ARGS__)
#define err(fmt, ...) do {		\
	warn(fmt, ##__VA_ARGS__);	\
	exit(1);			\
    } while (0);

    D("loading schedule file: %s", cfile);
    fin = fopen(cfile, "r");
    if (fin == NULL) {
	perror(cfile);
	exit(1);
    }
    for (ln = 1; fgets(buf, BUFSIZE, fin); ln++) {
	char **argv;
	int i, argc;
	struct _txp tmp, *t = &tmp;

	if (buf[0] == '#')
	    continue;

	argv = split_arg(buf, &argc);
	if (argc < 1)
		continue;
	if (strcmp(argv[0], "thread") == 0) {
	    char *ep;

	    if (argc < 2)
	    	err("missing thread number");
	    if (argc > 3)
	    	warn("extra arguments ignored");

	    if (strcmp(argv[1], "all") == 0) {
		tid = 0;
		thigh = f->n_threads;
	    } else {
		thigh = tid = strtol(argv[1], &ep, 0);
		if (*ep)
		    err("expecting number, got '%s'", argv[1]);
		if (argc >= 3) {
		    if (strcmp(argv[2], "*") == 0)
			thigh = f->n_threads - 1;
		    else
			thigh = strtol(argv[2], &ep, 0);
		    if (*ep)
			err("expecting number, got '%s'", argv[2]);
		}
		if (tid < 0 || tid >= f->n_threads ||
		    thigh < 0 || thigh >= f->n_threads || tid > thigh) {
		    warn("invalid thread '%ld', skipping...", tid);
		    tid = -1;
		}
	    }
	    goto next;
	}
	if (tid == -1) {
	    warn("skipped");
	    goto next;
	}
	*t = (struct _txp){ 0, 0, 0, 0, 1};
	if (atoi(argv[0]) < 0) {
	    t->duration = -1;
	} else {
	    t->duration = parse_time(argv[0]);
	    if (t->duration == U_PARSE_ERR)
		err("expecting duration, got '%s'", argv[0]);
	}
	if (t->duration > 0) {
	    int pps = 0;
	    if (argc < 2)
		err("missing rate");
	    t->duration /= 1000000000;
	    t->rate = parse_bw(argv[1]);
	    if (t->rate == U_PARSE_ERR)
		err("expecting rate, got '%s'", argv[1]);
	    pps = (argv[1][strlen(argv[1]) - 1] == 'p');
	    t->pkt_len = TXP_DEFLEN;
	    if (argc > 2) {
		uint64_t pkt_len = parse_qsize(argv[2]);
		if (pkt_len == U_PARSE_ERR)
		    err("expecting pkt len, got '%s'", argv[2]);
		if (pkt_len > (1UL << sizeof(t->pkt_len)*8) - 1)
		    err("pkt_len too big: '%s'", argv[2])
		t->pkt_len = pkt_len;
		t->burst_len = t->pkt_len;
		if (argc > 3) {
		    uint64_t burst_len = parse_qsize(argv[3]);
		    if (burst_len == U_PARSE_ERR)
			err("expecting burst len, got '%s'", argv[3]);
		    if (burst_len > (1UL << sizeof(t->burst_len)*8) - 1)
			err("burst len too big: '%s'", argv[3]);
		    t->burst_len = burst_len;
		    if (argc > 4)
			warn("extra arguments ignored");
		}
	    }
	    t->bpp = (pps == 0) ? t->pkt_len * 8 : 1;
	} else {
	    if (argc > 1)
		warn("extra argument ignored since duration = '%.2f'", t->duration);
	    goto next;
	}
	for (i = tid; i <= thigh; i++) {
            struct sched_td *cur = &f->td[i];
	    /* add a new entry to the worklist of current thread */
	    if (cur->wl_len == cur->wl_sz) {
		uint32_t newsz;
		if (cur->wl_sz >= SCHED_MAXSZ)
		    err("too many entries");
		newsz = (cur->wl_sz ? cur->wl_sz * 2 : 16);
		cur->worklist = realloc(cur->worklist, sizeof(*cur->worklist)*newsz);
		if (cur->worklist == NULL)
		    err("out of memory");
		cur->wl_sz = newsz;
	    }
	    cur->worklist[cur->wl_len] = *t;
	    cur->wl_len++;
	}
next:
	free(argv);
    }
    fclose(fin);
#undef BUFSIZE
#undef warn
#undef err
}

/* packet transmission time expressed in ticks */
static inline long int
pkt_tsc(struct sched_all *f, uint16_t len)
{
    return (f->bytes_to_tsc * len);
}


static uint64_t
ns2tsc(struct sched_all *f, unsigned long int ns)
{
    return (double)ns/1e9 * f->ticks_per_second;
}

static uint32_t
extract_marked(struct cqueue_client *q, struct sched_td *td,
               uint32_t max)
{
    uint32_t nmarked;
    
    if (rdtsc() < q->cli_extract_next) {
	return 0;
    }

    nmarked = cqueue_cli_get(q, max);

    /* Rate-limit the reads of s_head, in order to limit the worst
     * case cache misses. */
    q->cli_extract_next = rdtsc() + td->parent->client_interval_tsc;

    return nmarked;
}

static uint32_t do_sched(struct sched_all *f, uint64_t now);


/*
 * Xmit burst functions: various combinations
 */

/* Our scheduler with no real packet transmission,
 * busy wait scheduler mode. */
static uint32_t
cli_submit_sched_null(struct cqueue_client *cq, struct sched_td *td,
                      uint32_t burst_len, uint16_t pkt_len)
{
    uint32_t n;

    (void)td;

    for (n = 0; n < burst_len; n++) {
	    /* send a packet */
	    cqueue_cli_put(cq, (uint64_t)td->packet, pkt_len);
    }

    /* No explicit publishing, submitted packets are implicitely
     * submitted by writing the len field of queue slots. */

    return 0; /* No packets actually sent. */
}

/* Our scheduler with no real packet transmission,
 * distributed scheduler mode. */
static uint32_t
cli_submit_sched_null_distr(struct cqueue_client *cq, struct sched_td *td,
                                uint32_t burst_len, uint16_t pkt_len)
{
    struct sched_all *f = td->parent;
    uint32_t n;

    for (n = 0; n < burst_len; n++) {
	    cqueue_cli_put(cq, 0, pkt_len);
    }

    /* Notify the scheduler if needed. Otherwise try to do the scheduler
     * work. */
    if (cq->notify_sched) {
        uint64_t v = 1;
        int n = write(f->notifyfd, &v, sizeof(v));

        if (n != sizeof(v)) {
            D("Failed to notify the scheduler");
        }
        cq->notify_sched = 0;

    } else if (sem_trywait(&f->sch_lock) == 0) {
        uint64_t t0 = rdtsc();

        do_sched(f, t0);
        cq->cycles_sched += rdtsc() - t0;

        sem_post(&f->sch_lock);
    }

    return 0;
}

/* Not our scheduler with UDP transmission. */
static uint32_t
cli_submit_nosched_udp(struct cqueue_client *cq, struct sched_td *td,
                       uint32_t burst_len, uint16_t pkt_len)
{
    uint32_t n;
    int ret;

    for (n = 0; n < burst_len; n++) {
        if (td->timestamps) {
            uint64_t *p = (uint64_t *)td->packet;
            *p = td->my_id;
            p++;
            *p = rdtsc();
        }
        if (td->use_mmsg) {
            td->onemsg.iov_len = pkt_len - UDP_PKT_HLEN;
            ret = sendmmsg(td->sockfd, td->mmsg, td->use_mmsg, 0);
        } else {
            ret = send(td->sockfd, td->packet, pkt_len - UDP_PKT_HLEN, 0);
        }
        if (unlikely(ret == -1)) {
            D("send() failed");
            exit(1);
        }

        cq->n_cli_put ++;
        cq->n_cli_put_bytes += pkt_len;
        cq->n_cli_get ++;
    }

    return burst_len;
}

/*
 * Xmit clean functions: various combinations
 */

/* Our scheduler with no real packet transmission. */
static uint32_t
cli_xmit_clean_sched_null(struct cqueue_client *cq, struct sched_td *td,
                          uint16_t pkt_len)
{
    (void)pkt_len;
    return extract_marked(cq, td, ~0U);
}

/* Our scheduler with UDP transmission.  */
static uint32_t
cli_xmit_clean_sched_udp(struct cqueue_client *cq, struct sched_td *td,
                         uint16_t pkt_len)
{
    uint32_t c_head = cq->c_head; /* snapshot before calling extract_marked */
    uint32_t nmarked, i;
    int ret = 0;

    // XXX maybe we should only extract one
    nmarked = extract_marked(cq, td, ~0U);

    if (!td->active) {
        return nmarked;
    }

    for (i = 0; i < nmarked; i++) {
        if (td->timestamps) {
            uint64_t *p = (uint64_t *)td->packet;
            *p = td->my_id;
            p++;
            *p = cq->st[c_head].cli_enq;
        }

        ret = send(td->sockfd, td->packet, pkt_len - UDP_PKT_HLEN, 0);
        if (unlikely(ret == -1)) {
            RD(1, "send() failed: %s (len=%u)", strerror(errno), pkt_len - UDP_PKT_HLEN);
        }
        c_head = cqueue_next(cq->q, c_head);
    }

    return nmarked;
}

/* Our scheduler with netmap transmission.  */
static uint32_t
cli_xmit_clean_sched_netmap(struct cqueue_client *cq, struct sched_td *td,
                            uint16_t pkt_len)
{
    uint32_t c_head = cq->c_head; /* snapshot before calling extract_marked */
    struct nm_desc *nmd = td->nmd;
    struct netmap_ring *ring = NETMAP_TXRING(nmd->nifp, 0);
    unsigned int head = ring->head;
    uint32_t nmarked, i;
    int space, reclaim = 1;

    /* Compute available space in netmap ring in order to avoid
     * extracting marked packet that will be dropped */
again:
    space = nm_ring_space(ring);
    if (space == 0) {
        if (!reclaim) {

            return 0;
        }

        /* Try to reclaim packets. */
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
        reclaim = 0;
        goto again;
    }

    nmarked = extract_marked(cq, td, space);

    /* assert(nmarked <= space); */
    if (unlikely(nmarked > (uint32_t)space)) {
        D("BUG_ON(nmarked > space)");
        exit(1);
    }

    if (!td->active) {
        return nmarked;
    }

    for (i = 0; i < nmarked; i++) {
        struct netmap_slot *slot = ring->slot + head;

        slot->len = pkt_len;
        slot->flags = 0;
        // TODO: timestamps not compatible with real packet copy
        /*if (td->timestamps) {
            uint64_t *p = (uint64_t *)(NETMAP_BUF(ring, slot->buf_idx) + 14);
            *p = td->my_id;
            p++;
            *p = cq->st[c_head].cli_enq;
        }*/
        memcpy(NETMAP_BUF(ring, slot->buf_idx), td->packet,
                          sizeof(td->packet));

        head = nm_ring_next(ring, head);
        c_head = cqueue_next(cq->q, c_head);
    }

    if (i) {
        ring->head = ring->cur = head;
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
        cq->n_cli_io++;
    }

    return nmarked;
}

/* Not our scheduler with UDP transmission. */
static uint32_t
cli_xmit_clean_nosched_udp(struct cqueue_client *cq, struct sched_td *td,
                           uint16_t pkt_len)
{
    (void)cq;
    (void)td;
    (void)pkt_len;
    return 0;
}

/*
 * run one entry from the schedule.
 * td->task_start has the exact starting time of the entry
 * td->burst_start has the exact starting time for the burst
 * After sending a burst there is a silence until the rate is matched.
 *
 * On entry we are always above both start times
 */
static void
run_one(struct cqueue_client *cq, struct sched_td *td, struct _txp *t)
{
    cli_submit_t cli_submit = td->cli_submit;
    cli_xmit_clean_t cli_xmit_clean = td->cli_xmit_clean;
    uint64_t task_clocks, burst_clocks, my_tsc_rate;
    double act_bits, burst_sec = 0;
    uint32_t cycles, bursts, pkts;
    char *pps = (t->bpp == 1) ? "pps" : "bps";

    my_tsc_rate = td->parent->ticks_per_second;
    cycles = bursts = pkts = 0;
    task_clocks = t->duration * my_tsc_rate;
    ND("duration %f sec %llu ticks tps %llu",
	t->duration, (_P64)task_clocks, (_P64)my_tsc_rate);
    /* compute duration in seconds and ticks */
    if (t->rate) {
        burst_sec = (double)t->bpp * t->burst_len / t->rate;
    }
    burst_clocks = burst_sec * my_tsc_rate;

    while (rdtsc() - td->task_start < task_clocks) {
	cycles++;
	if (unlikely(t->rate == 0))
	    continue;
	bursts++;

        if (unlikely(++cq->api_calls == API_CALLS_THRESH)) {
            uint64_t now = rdtsc();

            if ((cq->cycles_sched << 2) > (now - cq->t_ntfy_check)) {
                cq->notify_sched = 1;
            }
            ND(1, "scheduling overhead %3.5f %%\n",
                  ((double)cq->cycles_sched * 100.0)/(now - cq->t_ntfy_check));
            cq->api_calls = 0;
            cq->cycles_sched = 0;
            cq->t_ntfy_check = now;
        }

	/*
 	 * Send a burst of packets. For our scheduler, this means enqueuing
         * packets, possibly sending those through an UDP socket or
         * netmap and publishing the tail update to the scheduler.
         *
         * pkts is only incremented in actual transmissions, which can
         * be in either cli_submit() or cli_xmit_clean()
         */
        pkts += cli_submit(cq, td, t->burst_len, t->pkt_len);

        do {
	    /* wait end of burst at given rate */
	    pkts += cli_xmit_clean(cq, td, t->pkt_len);
	} while (rdtsc() - td->burst_start < burst_clocks);
	td->burst_start += burst_clocks;
    }
    ND("total %d cycles %d bursts %d pkts", cycles, bursts, pkts);
    act_bits = (double)pkts*t->pkt_len*8;
    D("\t %2d %.3e bits %.3e bps %.3e pkts %.3e pps at %.3e %s %.3e drop/s "
          "%.3e avg_io_batch",
	td->my_id,
	act_bits, act_bits/t->duration, (double)pkts, pkts/t->duration, t->rate, pps,
	(double)cq->n_cli_drp/t->duration,
        cq->n_cli_io ? ((double)cq->n_cli_get/cq->n_cli_io) : 0.0);
    /* the actual task may be longer */
    td->task_start += task_clocks;
}


struct cqueue_client * cli_init(struct sched_td *ft) {
    struct cqueue_client *cq = SAFE_CALLOC(sizeof(*cq) + ft->q->real_size * sizeof(struct pkt_stat));
    cqueue_cli_init(cq, ft->q);
    return cq;
}

void cli_wait_init(struct sched_td *ft) {
    cqueue_wait_ready(ft->q);
}

/*
 * client code
 */
static void *
cli_mainloop(struct sched_td *ft)
{
    uint32_t cw;
    struct cqueue_client *cq = SAFE_CALLOC(sizeof(*cq) + ft->q->real_size * sizeof(struct pkt_stat));

    cqueue_cli_init(cq, ft->q);

    cqueue_wait_ready(ft->q);
    ft->task_start = rdtsc();
    cw = 0;
    while (cw < ft->wl_len) {
	struct _txp *t = &ft->worklist[cw];
	if (t->duration < 0)
	    break;
	if (t->duration == 0) {
	    cw = 0;
	    continue;
	}
	ft->burst_start = rdtsc();
	run_one(cq, ft, t);
        if (cq->n_cli_get) {
            D("avg delay = %"PRIu64" ns",
                    TSC2NS(cq->n_cli_lag / cq->n_cli_get));
        }
	cw++;
    }
    /* drain remaining traffic from the queue */
    while (!cqueue_cli_done(cq)) {
	ft->cli_xmit_clean(cq, ft, /* pkt_len=*/128);
    }

    return cq;
}

/*
 * A cache of mbufs which are needed by the scheduling algorithm.
 */
static void
mbuf_cache_init(struct mbuf_cache *c, uint32_t size)
{
    assert(size);

    c->first_free = c->cache = SAFE_CALLOC(sizeof(struct mbuf) * size);

    for (size--; size > 0; size--)
	c->cache[size - 1].m_freelist = c->cache + size;
}

static inline struct mbuf *
mbuf_cache_get(struct mbuf_cache *c)
{
    struct mbuf *m = c->first_free;
    c->first_free = m->m_freelist;
    return m;
}

static inline void
mbuf_cache_put(struct mbuf_cache *c, struct mbuf *m)
{
    m->m_freelist = c->first_free;
    c->first_free = m;
}

/*
 * Scheduler code
 */

static struct nm_desc *
netmap_init_realsched(const char *ifname)
{
    struct nm_desc *nmd; /* netmap descriptor */
    struct nmreq req;

    D("Opening scheduler netmap interface %s", ifname);

    memset(&req, 0, sizeof(req));
    nmd = nm_open(ifname, &req, NETMAP_NO_TX_POLL, NULL);
    if (nmd == NULL) {
        D("nm_open()");
        exit(1);
    }

    return nmd;
}

uint32_t netmap_ring_free_space(struct nm_desc *nmd) {
    struct netmap_ring *ring = NETMAP_TXRING(nmd->nifp, 0);
    int space, reclaim = 1;
again:
    space = nm_ring_space(ring);
    if (space == 0) {
        if (!reclaim) {

            return 0;
        }

        /* Try to reclaim packets. */
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
        reclaim = 0;
        goto again;
    }

    return space;
}

/* Scheduler iteration: returns the number of packets that have been
 * dequeued and marked. */
static uint32_t
do_sched(struct sched_all *f, uint64_t now)
{
    struct cqueue_sched *cqs = f->cqs;
    unsigned int i;
    uint32_t ndeq = 0;

    /* netmap output interface variables */
    struct nm_desc *nmd = f->nmd;
    struct netmap_ring *ring = NETMAP_TXRING(nmd->nifp, 0);
    unsigned int head = ring->head;
    uint32_t j;
    uint32_t space;

    /*
     * bring in pending packets, arrived between next_link_idle and now
     * (we assume they arrived at last_check)
     */
    for (i = 0; i < f->n_threads; i++) {
        struct mbuf *m;
        //uint32_t tot = 0, npkts = 0;
        uint32_t npkts = 0;
        /*
         * Skip clients with at least one packet/burst already in the
         * scheduler. This is true if s_nhead != s_tail, and
         * is a useful optimization.
         */
        if (cqueue_pending_mark(cqs + i)) {
            continue;
        }

        if (now < cqs[i].sch_extract_next) {
            continue;
        }
        cqs[i].sch_extract_next = now + f->sched_interval_tsc;

        cqueue_sched_fetch(cqs + i);

        /* peek one packet at time until queue is empty */
        for (;;) {
            /* peek a new packet and accumulate it if we
             * do not exceed the byte limit. When the limit
             * is exceeded, we enqueue any accumulated packets
             * and then peek the new packet again
             */
            // uint32_t sz = cqueue_sched_peek(cqs + i);
            // ND(3, "sz %u", sz);
            // if (sz && tot + sz <= f->sched_byte_limit) {
            //         tot += sz;
            //         npkts++;
            //         cqueue_sched_get(cqs + i);
            //         ND(3, "tot %u npkts %u, continuing", tot, npkts);
            //         continue;
            // }
            // /* special case for packets that exceed the byte limit
            //  * by themselves.
            //  */
            // if (unlikely(tot == 0 && sz > f->sched_byte_limit)) {
            //     cqueue_sched_get(cqs + i);
            //     npkts = 1;
            //     tot = sz;
            // }
            // if (tot) {
            //     m = mbuf_cache_get(&f->mbc);
            //     m->m_pkthdr.len = tot;
            //     m->npkts = npkts;
            //     tot = npkts = 0;
            //     m->flow_id = i;
            //     if (sched_enq(f->sched, m)) {
            //         /* dropped ! */
            //         RD(1, "DROP on queue %u", i);
            //         // XXX break
            //     }
            // }
            // if (!sz)
            //     break;


            uint32_t sz = cqueue_sched_peek(cqs + i);
            ND(3, "sz %u", sz);
            if (sz) {
                uint64_t bufptr = cqueue_sched_get(cqs + i);
                npkts++;

                m = mbuf_cache_get(&f->mbc);
                m->m_pkthdr.ptr = bufptr;
                m->m_pkthdr.len = sz;
                m->npkts = 1;
                m->flow_id = i;
                if (sched_enq(f->sched, m)) {
                    /* dropped ! */
                    RD(1, "DROP on queue %u", i);
                    // XXX break
                }
            }
            if (!sz)
                break;
        }

        /* mark extracted packets, so the client can update his tail */
        //cqueue_mark(cqs + i, npkts);
    }


    /* precompute available netmap ring space to avoid
     * dropping descheduled packets */
    space = netmap_ring_free_space(nmd);

    for (j = 0; j < space; j++) {
        /* packet rate limiter + batch limit */
        if(unlikely(!(f->next_link_idle <= now && ndeq < f->sched_batch_limit)))
            break;

        /* dequeue one packet */
        struct mbuf *m = sched_deq(f->sched);
        if (m == NULL)
            break;
        f->next_link_idle += pkt_tsc(f, m->m_pkthdr.len);
        ndeq++;

        /* mark packet to client as dequeued 
         * we do it here to keep max mbufs equal to sum of cqueue sizes */
        cqueue_mark(cqs + m->flow_id, 1);

        /* copy to netmap ring */
        struct netmap_slot *slot = ring->slot + head;
        slot->len = m->m_pkthdr.len;
        slot->flags = 0;
        //fprintf(stderr, "sched: dequeued pkt p=%lu, len=%u \n", m->m_pkthdr.ptr, m->m_pkthdr.len);
        memcpy(NETMAP_BUF(ring, slot->buf_idx), (void*)m->m_pkthdr.ptr,
            m->m_pkthdr.len);

        head = nm_ring_next(ring, head);

        /* free nbuf */
        mbuf_cache_put(&f->mbc, m);
    }

    if (j) {
        ring->head = ring->cur = head;
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
        //cq->n_cli_io++;
    }

    if (ndeq > 0) { /* publish updated heads */
        for (i = 0; i < f->n_threads; i++) {
            cqueue_sched_publish(cqs + i);
        }
    }



    // ND("now grabbing packets");
    // while (f->next_link_idle <= now && ndeq < f->sched_batch_limit) {
    //     struct mbuf *m = sched_deq(f->sched);
    //     if (m == NULL)
    //         break;
    //     cqueue_mark(cqs + m->flow_id, m->npkts);
    //     f->next_link_idle += pkt_tsc(f, m->m_pkthdr.len);
    //     mbuf_cache_put(&f->mbc, m);
    //     ndeq++;
    //     ND(1, "len %u tv_nsec %lu", m->m_pkthdr.len, ns);
    // }
    // ND("dequeued %u packets", ndeq);
    // if (ndeq > 0) { /* publish updated heads */
    //     for (i = 0; i < f->n_threads; i++) {
    //         cqueue_sched_publish(cqs + i);
    //     }
    // }

    return ndeq;
}

#define SCH_BUSY_WAIT_USECS     30
#define SCH_SLEEP_MSECS         500

/* Scheduler main loop. Repeatedly invokes do_sched() and carries out
 * other housekeeping tasks. */
static void
sched_mainloop(struct sched_all *f)
{
    uint64_t clients = 0, client;
    uint32_t *s_heads = NULL;
    struct cqueue_sched *cqs;
    unsigned startup = 1; /* only used for statistics */
    uint64_t t_last_deq; /* last time we did some dequeue work */
    uint64_t sch_busy_wait; /* how long to busy wait before going to sleep */
    uint32_t cs;
    uint32_t i;

    sem_init(&f->sch_lock, 0, 1);
    sem_wait(&f->sch_lock);

    s_heads = calloc(f->n_threads, sizeof(uint32_t));
    /* create the scheduler-side views of the cqueues */
    f->cqs = cqs = SAFE_CALLOC(sizeof(struct cqueue_sched) * f->n_threads);
    cs = 0; /* accumulate the total size of the cqueues */
    for (i = 0; i < f->n_threads; i++) {
	struct cqueue *q = f->td[i].q;
	cqueue_sched_init(cqs + i, q);
	cs += cqueue_size(q);
    }
    mbuf_cache_init(&f->mbc, cs);

    /* we keep a bitmap of the running clients, just to know
     * when we have to stop the simulation
     */
    for (i = 0, client = 1; i < f->n_threads; i++, client <<= 1) {
	cqueue_init_p_s_head(f->td[i].q, s_heads + i);
	clients |= client;
    }

    sch_busy_wait = ns2tsc(f, SCH_BUSY_WAIT_USECS * 1000);

    /* set the starting time, very important */
    f->next_link_idle = f->sched_start = t_last_deq = rdtsc();
    for (;;) {
	uint64_t now = rdtsc();
	uint32_t ndeq = do_sched(f, now);

	/*
	 *	next	ndeq
	 *	<=now	0	A: no more pkts, next=now, sleep till now+timeout
	 *	<=now	1..<lim	B: publish, no more pkts, next=now, sleep till now+timeout
	 *	<=now	lim	C: publish and retry
	 *	>now	0	D: ahead, sleep till next (or now+timeout?)
	 *	>now	1..<lim	E: publish, ahead, sleep till next (or now+timeout?)
	 *	>now	lim	E: publish, ahead, sleep till next (or now+timeout?)
	 */
        if (ndeq > 0) {
            t_last_deq = now;
            startup = 0;
        } else { /* XXX do less often */
	    /* update the bitmap of running clients */
	    f->stat_check_idle++;
	    for (i = 0, client = 1; i < f->n_threads; i++, client <<= 1) {
		if (clients & client) {
		    int ready = ACCESS_ONCE(f->td[i].ready);
		    if (!ready) {
			/* we have served all the queue and the client is
			 * not going to send more
			 */
			clients &= ~client;
		    }
		}
	    }
	    if (!clients)
		break;
	}

        if (unlikely(f->distributed_mode &&
                     now - t_last_deq > sch_busy_wait)) {
            struct pollfd pfd;
            int ret;

            sem_post(&f->sch_lock);
            pfd.fd = f->notifyfd;
            pfd.events = POLLIN;
            ret = poll(&pfd, 1, SCH_SLEEP_MSECS);
            if (ret < 0) {
                D("poll() failed");
                exit(1);
            }

            sem_wait(&f->sch_lock);

            /* Drain the eventfd if necessary. */
            if (ret > 0) {
                uint64_t v;
                int n = read(pfd.fd, &v, sizeof(v));
                if (n != sizeof(v)) {
                    D("Failed to drain the eventfd");
                }
                RD(1, "DRAINING EVENTFD %llu", (long long unsigned)v);
                (void)v;
            }
        }

	if (f->next_link_idle <= now) { /* XXX make it wrap-safe */
	    if (ndeq < f->sched_batch_limit) {
		f->next_link_idle = now; /* no traffic in this interval */
		if (unlikely(!startup)) {
                    /* ignore idle at startup */
		    f->stat_sched_idle++;
                }
		tsc_sleep_till(now + f->sched_interval_tsc);
	    }
	    f->stat_batch_full++;
	    /* else continue */
	} else {
	    f->stat_early++;
	    tsc_sleep_till(f->next_link_idle /* now + f->sched_interval_tsc */);
	}
    }
    f->sched_end = rdtsc();

    /* Collect statistics. */
    f->n_sch_pub = 0;
    f->n_sch_fetch = 0;
    for (i = 0; i < f->n_threads; i++) {
        struct cqueue_sched *cq = cqs + i;

        f->n_sch_pub += cq->n_sch_pub;
        f->n_sch_fetch += cq->n_sch_fetch;
    }

    free(s_heads);
    free(f->cqs);
    f->cqs = NULL;

    sem_post(&f->sch_lock);
    sem_destroy(&f->sch_lock);
}

/*
 * a variant of the runon function that remaps core numbers.
 */
void
td_runon(const char *name, u_int i, unsigned use_timestamps)
{
    static u_int core_map1[] = { 3, 0, 1, 2, 4, 5, 6, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 10, 11, 4};
    static u_int core_map2[] = { 0, 4, 1, 5, 2, 6, 3, 15, 4, 16, 5, 17, 6, 18, 7, 19, 8, 20, 9, 21, 10, 22, 11, 7};
    u_int *core_map = NULL;

    core_map = use_timestamps ? core_map1 : core_map2;

    if (i < sizeof(core_map1)/sizeof(u_int))
	    i = core_map[i];
    runon(name, i);
}

void
td_runon2(const char *name, u_int i, unsigned use_timestamps)
{
    const int cores[] = {0, 4, 1, 5, 2, 6};
    const int num_cores = sizeof(cores)/sizeof(cores[0]);

    (void) use_timestamps;
    runon(name, cores[i % num_cores]);
}

static void *
cli_body(void *_f)
{
    struct sched_td *ft = _f;
    struct cqueue_client *q;

    td_runon2(ft->name, ft->my_id, ft->timestamps);
    tsc_sleep_till(rdtsc() + ft->parent->ticks_per_second); /* spin up the core and freq */

    /* start reading from input */
    q = cli_mainloop(ft);
    ND("thread %d terminating", ft->my_id);
    return q;
}

static void*
sched_body(void *_f)
{
    struct sched_all *f = _f;

    runon("sched", f->sched_affinity);
    tsc_sleep_till(rdtsc() + f->ticks_per_second); /* spin up the core and freq */
    sched_mainloop(f);
    ND("scheduler terminating");
    return NULL;
}

static void
socket_init(struct sched_td *td)
{
    struct sockaddr_in addr;
    int port = 9302;
    int i;

    /* Initialize UDP socket for send(). */
    td->sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (td->sockfd < 0) {
        D("failed to open UDP socket");
        exit(1);
    }

    /* Bind the local port to allow easy traffic classification. */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port + 1000 + td->my_id);

    if (bind(td->sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        D("failed to bind()");
        exit(1);
    }

    if (td->parent->multi_udp_ports) {
        port += td->my_id;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_aton(td->parent->dst_ip_addr, &addr.sin_addr) == 0) {
        D("inet_aton() failed");
        exit(1);
    }

    if (connect(td->sockfd, (struct sockaddr *)&addr,
                sizeof(addr)) == -1) {
        D("failed to connect() UDP socket");
        exit(1);
    }

    td->onemsg.iov_base = td->packet;
    td->onemsg.iov_len = 0; /* to be filled at sendmmsg() time */
    memset(&td->mmsg, 0, sizeof(td->mmsg));
    for (i = 0; i < NMMSG; i++) {
        td->mmsg[i].msg_hdr.msg_iov = &td->onemsg;
        td->mmsg[i].msg_hdr.msg_iovlen = 1;
    }
}

static void
netmap_init(struct sched_td *td)
{
    struct netmap_ring *ring;
    struct nm_desc *nmd; /* netmap descriptor */
    char ifname[IFNAMSIZ];
    struct nmreq req;
    unsigned int i;

    snprintf(ifname, sizeof(ifname), "vale%u:1/T", td->my_id);
    D("Opening netmap interface %s", ifname);

    memset(&req, 0, sizeof(req));
    nmd = nm_open(ifname, &req, NETMAP_NO_TX_POLL, NULL);
    if (nmd == NULL) {
        D("nm_open()");
        exit(1);
    }

    td->nmd = nmd;

    /* Prepare the packets content in the TX ring. */
    ring = NETMAP_TXRING(nmd->nifp, 0);
    for (i = 0; i < ring->num_slots; i++) {
        struct netmap_slot *slot = ring->slot + i;

        memcpy(NETMAP_BUF(ring, slot->buf_idx), td->packet,
                          sizeof(td->packet));
    }
}

/*
 * main program: setup initial parameters and threads, run
 */
int
main(int ac, char **av)
{
    cli_submit_t cli_submit;
    cli_xmit_clean_t cli_xmit_clean;
    struct sched_all _f, *f = &_f;
    const char *cfile = "sched.txt";
    unsigned active_threads_set = 0;
    int open_netmap_port = 0;
    int open_udp_socket = 0;
    uint32_t qsize = 1024;
    uint32_t i;
    int ch;

    D("starting %s", av[0]);
    memset(f, 0, sizeof(*f));
    f->n_threads = 2;
    f->stat_prefix = "";

    f->ticks_per_second = calibrate_tsc();
    /* getopt etc */

    f->dst_ip_addr = "10.60.1.1";
    f->distributed_mode = 0;
    f->multi_udp_ports = 0;
    f->sched_interval = 5000; /* default: 5us */
    f->client_interval = 5000; /* default: 5us */
    f->sched_bw = 1e9; /* 1Gb/s */
    f->sched_affinity = 0;
    f->sched_byte_limit = 1500;
    f->sched_batch_limit = 500;
    f->timestamps = 0;
    f->use_mmsg = 0;
    cli_submit = cli_submit_sched_null;
    cli_xmit_clean = cli_xmit_clean_sched_null;

    while ( (ch = getopt(ac, av, "d:a:t:f:q:i:c:b:p:m:B:z:DGTk:M")) != -1) {
	switch (ch) {
        case 'd': /* destination IP address for UDP traffic */
            f->dst_ip_addr = optarg;
            break;
	case 'a': /* scheduler affinity */
	    f->sched_affinity = atoi(optarg);
	    break;
	case 't': /* threads */
	    f->n_threads = strtoul(optarg, NULL, 0);
	    break;
        case 'k': /* num_active threads */
            f->n_active_threads = strtoul(optarg, NULL, 0);
            active_threads_set = 1;
            break;
	case 'f': /* configuration file */
	    cfile = strdup(optarg);
	    break;
	case 'q': /* client queue size */
	    qsize = strtoul(optarg, NULL, 0);
	    break;
	case 'c': /* client interval (nanoseconds) */
	    f->client_interval = strtoul(optarg, NULL, 0);
	    break;
	case 'i': /* scheduler interval (nanoseconds) */
	    f->sched_interval = strtoul(optarg, NULL, 0);
	    break;
	case 'b': /* output bandwidth */
	    f->sched_bw = parse_bw(optarg);
	    break;
	case 'm': /* byte limit for multipackets */
        /* disabled */
        f->sched_byte_limit = parse_qsize(optarg);
        fprintf(stderr, "Sched_byte_limit unsupported!\n");
	    break;
	case 'B': /* batch limit for the scheduler */
	    f->sched_batch_limit = strtoul(optarg, NULL, 0);
	    break;
	case 'p': /* prefix for stat files */
	    f->stat_prefix = strdup(optarg);
	    break;
        case 'z': /* client transmission type (UDP, netmap, scheduler, etc) */
            if (strcmp(optarg, "sched_null") == 0) {
                cli_submit = cli_submit_sched_null;
                cli_xmit_clean = cli_xmit_clean_sched_null;
            } else if (strcmp(optarg, "sched_udp") == 0) {
                cli_submit = cli_submit_sched_null;
                cli_xmit_clean = cli_xmit_clean_sched_udp;
                open_udp_socket = 1;
            } else if (strcmp(optarg, "nosched_udp") == 0) {
                cli_submit = cli_submit_nosched_udp;
                cli_xmit_clean = cli_xmit_clean_nosched_udp;
                open_udp_socket = 1;
                f->distributed_mode = 1;
            } else if (strcmp(optarg, "sched_netmap") == 0) {
                cli_submit = cli_submit_sched_null;
                cli_xmit_clean = cli_xmit_clean_sched_netmap;
                open_netmap_port = 1;
            } else if (strcmp(optarg, "sched_real_netmap") == 0) {
                cli_submit = cli_submit_sched_null;
                cli_xmit_clean = cli_xmit_clean_sched_null;
            }
            break;
        case 'D': /* enable distributed mode */
            f->distributed_mode = 1;
            break;
        case 'G': /* with UDP sockets, don't use the same destination
                   * port for all the threads */
            f->multi_udp_ports = 1;
            break;
        case 'T': /* timestamps packets to measure latency */
            f->timestamps = 1;
            break;
        case 'M':
            f->use_mmsg = strtoul(optarg, NULL, 0);
            if (f->use_mmsg > NMMSG) {
		f->use_mmsg = NMMSG;
            }
            break;
	}
    }
    if (optind > 0) {
	ac -= optind - 1;
	av += optind - 1;
    }

    if (!active_threads_set || f->n_active_threads > f->n_threads) {
        f->n_active_threads = f->n_threads;
    }

    if (cli_submit == cli_submit_sched_null && f->distributed_mode) {
        cli_submit = cli_submit_sched_null_distr;
    }

    f->bytes_to_tsc = 8.0 * f->ticks_per_second / f->sched_bw;

    f->sched_interval_tsc = ns2tsc(f, f->sched_interval);
    f->client_interval_tsc = ns2tsc(f, f->client_interval); /* wait between unsched retries */

    f->td = SAFE_CALLOC(f->n_threads * sizeof(*f->td));

    f->notifyfd = eventfd(0, 0);
    if (f->notifyfd < 0) {
        D("Failed to open eventfd() (not fatal)");
    }

    /* load_schedule() initializes some fields in f->td[i]. */
    load_schedule(f, cfile);
    print_schedule(f);

    /* Initialize the remainder of f->td[i], but do not start
     * the client threads yet. */
    for (i = 0; i < f->n_threads; i++) {
	struct sched_td *td = &f->td[i];
	td->my_id = i;
	td->parent = f;
        td->qsize = qsize;
        td->q = NULL; /* allocated by client pthreads */
        td->timestamps = f->timestamps;
        td->use_mmsg = f->use_mmsg;
        td->active = (i < f->n_active_threads);
	snprintf(td->name, sizeof(td->name) - 1, "cli-%02d", i);
        if (open_udp_socket) {
            socket_init(td);
        } else if (open_netmap_port) {
            netmap_init(td);
        }
        td->cli_submit = cli_submit;
        td->cli_xmit_clean = cli_xmit_clean;
        if (open_netmap_port) {
            packet_eth_init(td->packet);
        }
        /* Make sure cqueue is allocated on the same socket as the client. */
        td_runon2(td->name, td->my_id, td->timestamps);
        td->q = cqueue_create(td->qsize, "queue-%u", td->my_id);
	td->ready = 1;
    }

    /* Init scheduler thread and run it. f->td[i] must be
     * initialized here. */
    f->sched = sched_init(ac, av);
    f->nmd = netmap_init_realsched("vale0:1");
    if (f->sched == NULL) {
	fprintf(stderr, "failed to create the scheduler\n");
	exit(1);
    }
    f->stop = 0;
    pthread_create(&f->sched_id, NULL, sched_body, f);

    /* Start the client threads. */
    for (i = 0; i < f->n_threads; i++) {
	struct sched_td *td = &f->td[i];
	pthread_create(&td->td_id, NULL, cli_body, td);
    }

    {
	uint64_t pkts = 0, bytes = 0, cli_slow_read = 0;
	double duration;
	D("waiting for input to terminate");
	for (i = 0; i < f->n_threads; i++) {
	    struct cqueue_client *q;

	    pthread_join(f->td[i].td_id, (void **)&q);
	    ND("thread %d gone, q %p", i, q);
	    f->td[i].ready = 0;
	    pkts += q->n_cli_put;
	    bytes += q->n_cli_put_bytes;
            cli_slow_read += q->n_cli_slow_read;
	    ND("q->c_tail %u q->c_head %u q->c_shead %u",
		    q->c_tail, q->c_head, q->c_shead);
	    //free(q);
	}
        {
            /* At this point the scheduler thread may be blocked in the poll()
             * syscall. We need to wake it up here so that it can exit its main
             * loop (since the ready flags of the clients have been reset) and
             * compute sched_end timely, for the accuracy of rate computation
             * (otherwise we would have SCH_SLEEP_MSECS milliseconds added
             * to the denominator).  */
            uint64_t v = 1;
            int n = write(f->notifyfd, &v, sizeof(v));
            if (n != sizeof(v)) {
                D("Failed to notify the scheduler");
            }
        }
	D("waiting for scheduler to terminate");
	pthread_join(f->sched_id, NULL);
	duration = (double)(f->sched_end - f->sched_start)/f->ticks_per_second;
	D("sched_idle: %u", (u_int)f->stat_sched_idle);
	D("check_idle: %u", (u_int)f->stat_check_idle);
	D("stat_early: %u", (u_int)f->stat_early);
        D("STATS: cli_slow_read %.3e Hz, sch_pub %.3e Hz, sch_fetch %.3e Hz",
          (double)cli_slow_read/duration,
          (double)f->n_sch_pub/duration,
          (double)f->n_sch_fetch/duration);
	D("TOTAL: %.3e bits %.3e bps %.3e pkts %.3e pps",
	  8.0*bytes, 8.0*bytes/duration, (double)pkts, pkts/duration);
    }

#if 0
    for (i = 0; i < f->n_threads; i++) {
	char fname[MAXFNAME + 1];
	FILE *qf;
	struct cqueue *q = f->td[i].q;

	cqueue_show_counters(q);
	snprintf(fname, MAXFNAME, "%s%s.txt", f->stat_prefix, q->name);
	qf = fopen(fname, "w");
	if (!qf) {
	    perror(fname);
	    continue;
	}
	D("dumping %s timestamps into %s", q->name, fname);
	cqueue_dump_stats(q, qf);
	fclose(qf);	
    }
#endif

    D("all done");
    return 0;
}
