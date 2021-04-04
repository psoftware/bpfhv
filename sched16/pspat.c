//#include "backend.h"
#include "sched16.h"
#include <sys/ioctl.h>
#include <assert.h>

#define NETMAP_WITH_LIBS
#include <libnetmap.h>

#include "pspat.h"

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

uint32_t netmap_ring_free_space(struct nm_desc *nmd, struct netmap_ring *ring) {
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

#define SCH_BUSY_WAIT_USECS     30
#define SCH_SLEEP_MSECS         500

#define TXI_BEGIN(_s)   (_s)->num_queue_pairs
#define TXI_END(_s)     (_s)->num_queues

uint32_t
sched_dequeue_sink(struct sched_all *f, uint64_t now) {
    uint32_t ndeq = 0;

    while (f->next_link_idle <= now && ndeq < f->sched_batch_limit) {
        /* dequeue one packet */
        struct mbuf *m = sched_deq(f->sched);
        if (m == NULL)
            break;
        f->next_link_idle += pkt_tsc(f, m->iov.iov_len);
        ndeq++;

        f->n_sch_released_bytes += m->iov.iov_len;

        /* mark packet to client as dequeued (release it)
         * we do it here to keep max mbufs equal to sum of cqueue sizes */
        m->be->ops.txq_release(m->be, m->txq, m->idx);

        /* free mbuf */
        mbuf_cache_put(&f->mbc, m);
    }

    f->n_sch_released += ndeq;

    return ndeq;
}

uint32_t
sched_dequeue_netmap(struct sched_all *f, uint64_t now) {
    uint32_t ndeq = 0;

    /* netmap output interface variables */
    struct nm_desc *nmd = f->nmd;
    struct netmap_ring *ring = NETMAP_TXRING(nmd->nifp, 0);
    unsigned int head = ring->head;
    uint32_t j,space;

    /* precompute available netmap ring space to avoid
     * dropping descheduled packets */
    space = netmap_ring_free_space(nmd, ring);

    for (j = 0; j < space; j++) {
        /* packet rate limiter + batch limit */
        if(unlikely(!(f->next_link_idle <= now && ndeq < f->sched_batch_limit)))
            break;

        /* dequeue one packet */
        struct mbuf *m = sched_deq(f->sched);
        if (m == NULL)
            break;
        f->next_link_idle += pkt_tsc(f, m->iov.iov_len);
        ndeq++;

        f->n_sch_released_bytes += m->iov.iov_len;

        /* copy to netmap ring */
        struct netmap_slot *slot = ring->slot + head;
        slot->len = m->iov.iov_len;
        slot->flags = 0;
        //fprintf(stderr, "sched: dequeued pkt p=%lu, len=%u \n", m->m_pkthdr.ptr, m->iov.iov_len);
        memcpy(NETMAP_BUF(ring, slot->buf_idx), (void*)m->iov.iov_base,
            m->iov.iov_len);

        head = nm_ring_next(ring, head);

        /* mark packet to client as dequeued (release it)
         * we do it here to keep max mbufs equal to sum of cqueue sizes */
        m->be->ops.txq_release(m->be, m->txq, m->idx);

        /* free nbuf */
        mbuf_cache_put(&f->mbc, m);
    }

    if (ndeq > 0) {
        ring->head = ring->cur = head;
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
        //cq->n_cli_io++;
        /* update stats */
        f->n_sch_released += ndeq;
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
    //     for (i = 0; i < f->n_clients; i++) {
    //         cqueue_sched_publish(cqs + i);
    //     }
    // }

    return ndeq;
}

uint32_t
sched_dequeue(struct sched_all *f, uint64_t now) {
    return f->sched_deq_f(f,now);
}

uint32_t
fun_sched_enqueue(struct BpfhvBackendProcess *bp, struct BpfhvBackend *be, BpfhvBackendQueue *txq,
                             struct iovec iov, uint64_t opaque_idx, uint32_t mark) {
    ND(3, "sz %u", iov.iov_len);
    struct sched_all *f = bp->sched_f;

    if(unlikely(mark >= f->max_mark))
        return 1;

    /* be, txq and opaque_idx are needed to eventually release buf */
    struct mbuf *m = mbuf_cache_get(&f->mbc);
    m->iov = iov;
    m->be = be;
    m->txq = txq;
    m->idx = opaque_idx;
    m->flow_id = mark;

    /* enqueuing = fetching from client */
    f->n_sch_fetch++;

    return sched_enq(f->sched, m);
}

/* Scheduler iteration: fetch from clients */
// static void
// do_sched(struct sched_all *f, uint64_t now)
// {
//     struct cqueue_sched *cqs = f->cqs;
//     unsigned int i;

//     /*
//      * bring in pending packets, arrived between next_link_idle and now
//      * (we assume they arrived at last_check)
//      */
//     for (i = 0; i < f->n_clients; i++) {
//         struct mbuf *m;
//         uint32_t npkts = 0;
//         /*
//          * Skip clients with at least one packet/burst already in the
//          * scheduler. This is true if s_nhead != s_tail, and
//          * is a useful optimization.
//          */
//         /* TODO: try to reimplement this optimization. In our case
//          * mark = guest has not yet consumed released buffers */
//         /*if (cqueue_pending_mark(cqs + i)) {
//             continue;
//         }*/

//         if (now < cqs[i].sch_extract_next) {
//             continue;
//         }
//         cqs[i].sch_extract_next = now + f->sched_interval_tsc;

//         /* peek one packet at time until queue is empty */
//         for (;;) {
//             /* peek a new packet and accumulate it */
//             uint32_t sz = cqueue_sched_peek(cqs + i);
//             ND(3, "sz %u", sz);
//             if (sz) {
//                 //uint64_t bufptr = cqueue_sched_get(cqs + i);
//                 npkts++;

//                 m = mbuf_cache_get(&f->mbc);
//                 //m->m_pkthdr.ptr = bufptr;
//                 //m->m_pkthdr.len = sz;
//                 m->flow_id = i;
//                 if (sched_enq(f->sched, m)) {
//                     /* dropped ! */
//                     RD(1, "DROP on queue %u", i);
//                     // XXX break
//                 }
//             }
//             if (!sz)
//                 break;
//         }
//     }
// }

int
sched_dump(struct sched_all *f) {
    return dump(f->sched);
}

void
sched_idle_sleep(struct sched_all *f, uint64_t now, uint32_t ndeq) {
    /*
     *  next    ndeq
     *  <=now   0   A: no more pkts, next=now, sleep till now+timeout
     *  <=now   1..<lim B: publish, no more pkts, next=now, sleep till now+timeout
     *  <=now   lim C: publish and retry
     *  >now    0   D: ahead, sleep till next (or now+timeout?)
     *  >now    1..<lim E: publish, ahead, sleep till next (or now+timeout?)
     *  >now    lim E: publish, ahead, sleep till next (or now+timeout?)
     */

    if (f->next_link_idle <= now) { /* XXX make it wrap-safe */
        if (ndeq < f->sched_batch_limit) {
            f->next_link_idle = now; /* no traffic in this interval */
            f->stat_sched_idle++;
            tsc_sleep_till(now + f->sched_interval_tsc);
        }
        f->stat_batch_full++;
    /* else continue */
    } else {
        f->stat_early++;
        tsc_sleep_till(f->next_link_idle /* now + f->sched_interval_tsc */);
    }
}

/* Scheduler main loop. Repeatedly invokes do_sched() and carries out
 * other housekeeping tasks. */
// static void
// sched_do_sched_iter(struct sched_all *f)
// {    
//     uint64_t now = rdtsc();
//     /* client --> scheduler queues */
//     do_sched(f, now);

//     /* scheduler queues -> nic */
//     uint32_t ndeq = sched_dequeue(f, now, bp);

//     /* sleep to match f->sched_interval_tsc */
//     sched_idle_sleep(f, now);
// }

// static void*
// sched_body(void *_f)
// {
//     struct sched_all *f = _f;

//     runon("sched", f->sched_affinity);
//     tsc_sleep_till(rdtsc() + f->ticks_per_second); /* spin up the core and freq */

//     for(;;) {
//         /* do rx fetch */

//         /* do sched_iter */
//         sched_do_sched_iter(f);
//     }


//     ND("scheduler terminating");
//     return NULL;
// }

void sched_all_start(struct sched_all *f, uint32_t num_mbuf) {
    /* create the scheduler-side views of the cqueues */
    f->cqs = SAFE_CALLOC(sizeof(struct cqueue_sched) * f->n_clients);
    mbuf_cache_init(&f->mbc, num_mbuf);

    /* set the starting time, very important */
    f->next_link_idle = rdtsc();
}

void sched_all_finish(struct sched_all *f) {
    f->sched_end = rdtsc();

    /* ignore 1 idle at startup */
    if(f->stat_sched_idle != 0)
        f->stat_sched_idle--;

    /* print stats */
    {
        uint64_t pkts = f->n_sch_released, bytes = f->n_sch_released_bytes;
        double duration;
        D("waiting for scheduler to terminate");
        pthread_join(f->sched_id, NULL);
        duration = (double)(f->sched_end - f->sched_start)/f->ticks_per_second;
        D("sched_idle: %u", (u_int)f->stat_sched_idle);
        D("check_idle: %u", (u_int)f->stat_check_idle);
        D("stat_early: %u", (u_int)f->stat_early);
        D("TOTAL: %.3e bits %.3e bps %.3e pkts %.3e pps",
          8.0*bytes, 8.0*bytes/duration, (double)pkts, pkts/duration);
    }

    free(f->cqs);
    f->cqs = NULL;
    free(f);
}

struct sched_all *sched_all_create(int ac, char *av[], const char *ifname, uint iftype) {
    struct sched_all *f = SAFE_CALLOC(sizeof(struct sched_all));

    unsigned active_threads_set = 0;

    D("starting %s", av[0]);
    memset(f, 0, sizeof(*f));
    f->n_clients = 2;
    f->stat_prefix = "";

    f->ticks_per_second = calibrate_tsc();

    f->dst_ip_addr = "10.60.1.1";
    f->multi_udp_ports = 0;
    f->sched_interval = 5000; /* default: 5us */
    f->client_interval = 5000; /* default: 5us */
    f->sched_bw = 1e9; /* 1Gb/s */
    f->sched_affinity = 0;
    f->sched_byte_limit = 1500;
    f->sched_batch_limit = 500;
    f->use_mmsg = 0;

    switch(iftype) {
        case PSPAT_IF_TYPE_SINK:
            f->sched_deq_f = sched_dequeue_sink; break;
        case PSPAT_IF_TYPE_NETMAP:
            f->sched_deq_f = sched_dequeue_netmap; break;
        default:
            fprintf(stderr, "sched_all_create: invalid iftype\n");
            return NULL;
    }

    if (!active_threads_set || f->n_active_threads > f->n_clients) {
        f->n_active_threads = f->n_clients;
    }

    f->bytes_to_tsc = 8.0 * f->ticks_per_second / f->sched_bw;

    f->sched_interval_tsc = ns2tsc(f, f->sched_interval);
    f->client_interval_tsc = ns2tsc(f, f->client_interval); /* wait between unsched retries */

    /* Init scheduler thread and run it. f->td[i] must be
     * initialized here. */
    f->sched = sched_init(ac, av);
    f->max_mark = get_flow_count(f->sched);
    f->nmd = netmap_init_realsched(ifname);
    if (f->sched == NULL) {
        fprintf(stderr, "failed to create the scheduler\n");
        return NULL;
    }
    f->stop = 0;

    return f;
}

/*
 * main program: setup initial parameters and threads, run
 */
// int
// main(int ac, char **av)
// {
//     struct sched_all _f, *f = &_f;
//     unsigned active_threads_set = 0;
//     int ch;

//     D("starting %s", av[0]);
//     memset(f, 0, sizeof(*f));
//     f->n_clients = 2;
//     f->stat_prefix = "";

//     f->ticks_per_second = calibrate_tsc();
//     /* getopt etc */

//     f->dst_ip_addr = "10.60.1.1";
//     f->multi_udp_ports = 0;
//     f->sched_interval = 5000; /* default: 5us */
//     f->client_interval = 5000; /* default: 5us */
//     f->sched_bw = 1e9; /* 1Gb/s */
//     f->sched_affinity = 0;
//     f->sched_byte_limit = 1500;
//     f->sched_batch_limit = 500;
//     f->use_mmsg = 0;

//     while ( (ch = getopt(ac, av, "d:a:t:f:q:i:c:b:p:m:B:z:DGTk:M")) != -1) {
//         switch (ch) {
//             case 'd': /* destination IP address for UDP traffic */
//                 f->dst_ip_addr = optarg;
//                 break;
//             case 'a': /* scheduler affinity */
//                 f->sched_affinity = atoi(optarg);
//                 break;
//             case 't': /* threads */
//                 f->n_clients = strtoul(optarg, NULL, 0);
//                 break;
//                 case 'k': /* num_active threads */
//                     f->n_active_threads = strtoul(optarg, NULL, 0);
//                     active_threads_set = 1;
//                     break;
//             case 'c': /* client interval (nanoseconds) */
//                 f->client_interval = strtoul(optarg, NULL, 0);
//                 break;
//             case 'i': /* scheduler interval (nanoseconds) */
//                 f->sched_interval = strtoul(optarg, NULL, 0);
//                 break;
//             case 'b': /* output bandwidth */
//                 f->sched_bw = parse_bw(optarg);
//                 break;
//             case 'm': /* byte limit for multipackets */
//                 /* disabled */
//                 f->sched_byte_limit = parse_qsize(optarg);
//                 fprintf(stderr, "Sched_byte_limit unsupported!\n");
//                 break;
//             case 'B': /* batch limit for the scheduler */
//                 f->sched_batch_limit = strtoul(optarg, NULL, 0);
//                 break;
//             case 'p': /* prefix for stat files */
//                 f->stat_prefix = strdup(optarg);
//                 break;
//             case 'z': /* client transmission type (UDP, netmap, scheduler, etc) */
//                 /* netmap only for now */
//                 break;
//             case 'G': /* with UDP sockets, don't use the same destination
//                        * port for all the threads */
//                 f->multi_udp_ports = 1;
//                 break;
//             case 'M':
//                 f->use_mmsg = strtoul(optarg, NULL, 0);
//                 if (f->use_mmsg > NMMSG) {
//                     f->use_mmsg = NMMSG;
//                 }
//                 break;
//             }
//     }
//     if (optind > 0) {
//         ac -= optind - 1;
//         av += optind - 1;
//     }

//     if (!active_threads_set || f->n_active_threads > f->n_clients) {
//         f->n_active_threads = f->n_clients;
//     }

//     f->bytes_to_tsc = 8.0 * f->ticks_per_second / f->sched_bw;

//     f->sched_interval_tsc = ns2tsc(f, f->sched_interval);
//     f->client_interval_tsc = ns2tsc(f, f->client_interval); /* wait between unsched retries */


//     /* Init scheduler thread and run it. f->td[i] must be
//      * initialized here. */
//     f->sched = sched_init(ac, av);
//     f->nmd = netmap_init_realsched("vale0:1");
//     if (f->sched == NULL) {
//         fprintf(stderr, "failed to create the scheduler\n");
//         exit(1);
//     }
//     f->stop = 0;
//     pthread_create(&f->sched_id, NULL, sched_body, f);


    

//     D("all done");
//     return 0;
// }
