#ifndef CQUEUE_H_
#define CQUEUE_H_

/*
 * Per packet statistics.
 * For the time being we only store enqueue and dequeue timestamps
 * on the client. This is stored on a separate array only visible to
 * the client.
 */
struct pkt_stat {
    uint64_t	cli_enq; /* rdtsc() of client enqueue */
};

extern size_t cqueue_stat_size;

#define START_NEW_CACHELINE __attribute__((aligned(64)))

/*
 * The queue shared between client and scheduler only contains lengths.
 * The client adds non-zero lengths to the tail, which the scheduler
 * detects so the the tail pointer is published implicitly.
 *
 * A cacheline contains multiple entries, so the scheduler caches
 * the last row to avoid repeated reads (with associated cache
 * thrashing).
 * The scheduler marks packets by advancing s_head to the next packet
 * to mark. Clients read that variable and clear the field once
 * the packet has been actually transmitted.
 *
          c_head -->[ C   ]
                    [     ]
          c_shead-->[ C   ]		cached copy of s_head
                    [ C S ]<-- s_head	exported s_head
                    [   S ]<-- s_nhead	local copy
                    [     ]
                    [     ]
                    [   S ]<-- s_tail	argument for next enq()
                    [   S ]<-- s_ctail  cached copy of c_tail
          c_tail -->[ C   ]		c_tail
 */

struct cqueue;
struct cqueue_client {
    uint32_t n_pending; /* number of pending elements in the queue */
    uint32_t c_tail;    /* next slot to use for put operation */
    uint32_t c_head;	/* next packet to send down */
    uint32_t c_shead;	/* cached value of s_head */
    uint64_t n_cli_put;	/* packets */
    uint64_t n_cli_put_bytes;	/* bytes */
    uint64_t n_cli_get;
    uint64_t n_cli_io;
    uint64_t n_cli_lag; /* total sum of lags */
    uint64_t n_cli_drp;
    uint64_t n_cli_slow_read; /* Number of different values read from s_head */
    uint64_t cli_extract_next; /* tsc value for next extract retry */

#define API_CALLS_THRESH    128
    uint64_t cycles_sched; /* account for time spent doing scheduler work */
    uint64_t t_ntfy_check; /* last time we did scheduler notification check */
    uint64_t api_calls;    /* client API calls since t_ntfy_check */
    uint32_t notify_sched; /* we had better notify the scheduler asap */

    struct cqueue *q;
    struct pkt_stat st[0]; /* statistics for the corresponding slot in the queue */
};

#define PKT_PADDING 3
struct my_pkt {
    uint64_t bufptr;
    uint16_t len;
    uint16_t pad[PKT_PADDING];
};

/* Size of a cache line (must be a power of 2). */
#define QCACHE_SIZE     64U
/* Number of queue entries for each cache line. */
#define QCACHE_NUM      (QCACHE_SIZE/sizeof(struct my_pkt))

#define ct_assert(e) extern char (*ct_assert(void)) [sizeof(char[1 - 2*!(e)])]

ct_assert( (QCACHE_SIZE % sizeof(struct my_pkt)) == 0);

struct cqueue_sched {
    uint32_t s_nhead;	/* next packet to mark */
    uint32_t s_tail;	/* start of next cacheline to copy
		         * from cqueue to qcache
			 */
    uint32_t s_next;  /* next packet to enq() from qcache */
    uint32_t s_cache_valid; /* cache can be accessed (peek, get) */
    uint64_t n_sch_put;
    uint64_t n_sch_get;
    uint64_t n_sch_drp;
    uint64_t n_sch_pub; /* Number of writes to s_head */
    uint64_t n_sch_fetch; /* Number of fetches into the cache */
    uint64_t sch_extract_next; /* tsc value for next extract retry */

    START_NEW_CACHELINE
    struct my_pkt qcache[QCACHE_NUM];

    struct cqueue *q;
};

// StaticAssert( (64 % sizeof(struct my_pkt)) == 0);

struct cqueue {
#define CQUEUE_MAXNAME 60
    char name[CQUEUE_MAXNAME];
    uint32_t user_size;   /* specified by the user */
    uint32_t real_size;   /* allocated length */

    /* Shared memory cache line scheduler --> client,
     * used to publish s_head. */
    START_NEW_CACHELINE
    uint32_t _s_head;
    uint32_t *p_s_head; /* pointer to the s_head */

#if 0
    struct pkt_stat *stats;
    uint64_t stat_last_seq;
    uint64_t stat_size;
#endif

    START_NEW_CACHELINE
    struct my_pkt queue[0];
};

static inline uint32_t cqueue_size(struct cqueue *q)
{
    return q->user_size;
}

static inline uint32_t cqueue_next(struct cqueue *q, uint32_t i)
{
    i++;
    if (unlikely(i == q->real_size))
	i = 0;
    return i;
}

#define ACCESS_ONCE(v)	*(volatile typeof(v)*)&(v)
#define mb() __sync_synchronize()

struct cqueue * cqueue_create(uint32_t user_size, const char *fmt, ...);
void cqueue_cli_init(struct cqueue_client *, struct cqueue *);
void cqueue_sched_init(struct cqueue_sched *, struct cqueue *);

void cqueue_wait_ready(struct cqueue *);

/*
 * There are two possible strategies to store s_head for the different
 * clients. With the first strategy we store all the s_head in a
 * memory block (p argument), which will fit in one (or more) cachelines.
 * With the second strategy, we store each s_head in a separate cacheline.
 * From experiments, for both single socket and multi socket machines,
 * it seems that the first strategy is faster when there are multiple
 * clients, while the second one is faster whene there is just one
 * client.
*/
static inline void cqueue_init_p_s_head(struct cqueue *q, uint32_t *p)
{
    if (1) {
        q->p_s_head = p;
    } else {
        q->p_s_head = &q->_s_head;
    }
}


void cqueue_cli_put(struct cqueue_client *q, uint64_t buf, uint16_t len);

uint16_t cqueue_cli_get(struct cqueue_client *q, uint32_t max);
int cqueue_cli_done(struct cqueue_client *q);

/* return true if the queue has packet not marked yet on the scheduler side */
static inline int cqueue_pending_mark(struct cqueue_sched *q)
{
    return (q->s_nhead != q->s_tail + q->s_next);
}

void cqueue_mark(struct cqueue_sched *q, uint16_t npkts);
//void cqueue_show_counters(struct cqueue *q);
//void cqueue_dump_stats(struct cqueue *q, FILE *f);

/* scheduler: update the local cache, rate-limited */
static inline void
cqueue_sched_fetch(struct cqueue_sched *q)
{
    /* update the local cache */
    memcpy(q->qcache, q->q->queue + q->s_tail, QCACHE_SIZE);
    q->s_cache_valid = 1;
    q->n_sch_fetch ++;
}

/* scheduler: return the size of the next packet to enqueue, if any */
static inline uint16_t
cqueue_sched_peek(struct cqueue_sched *q)
{
    if (!q->s_cache_valid) {
        return 0;
    }

    return q->qcache[q->s_next].len;
}

/* scheduler: extract the next packet to enqueue */
static inline uint64_t
cqueue_sched_get(struct cqueue_sched *q)
{
    uint64_t buf;
    if(unlikely(!q->s_cache_valid)) {
        buf = 0;
    } else {
        buf = q->qcache[q->s_next].bufptr;
    }

    q->s_next++;
    if (q->s_next == QCACHE_NUM) {
        q->s_tail += QCACHE_NUM;
    	if (unlikely(q->s_tail >= q->q->real_size)) {
    	    q->s_tail -= q->q->real_size;
        }
        q->s_next = 0;
        q->s_cache_valid = 0;
    }

    return buf;
}

/* make s_head available. */
static inline void cqueue_sched_publish(struct cqueue_sched *_q)
{
    struct cqueue *q = _q->q;
    if (*q->p_s_head != _q->s_nhead) {
	ACCESS_ONCE(*q->p_s_head) = _q->s_nhead;
        _q->n_sch_pub++;
    }
}

#endif /* CQUEUE_H_ */
