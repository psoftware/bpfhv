/*
 * BSD Copyright
 */

/*
 * common headers for sched
 */

#ifndef __SCHED16_H__
#define __SCHED16_H__

#define _GNU_SOURCE	/* pthread_setaffinity_np */

/* XXX where ? */
#ifndef likely
#define likely(x)       __builtin_expect((x),1)
#endif
#ifndef unlikely
#define unlikely(x)     __builtin_expect((x),0)
#endif

#include <stdint.h>
#include <errno.h> // errno ?
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>	/* memcpy */
#include <signal.h>

#include <pthread.h>
#include <semaphore.h>
#include <sched.h> // linux ? CPU_ZERO

/*
 * locks and affinity are not completely portable.
 When possible we use pthread functions, but types slightly differ so
 - cpuset_t is the choice (from FreeBSD; linux uses cpu_set_t,
   OSX has nothing ?

 */

#define _P64	unsigned long long	/* cast to print 64-bit numbers with %llu */
#ifdef __FreeBSD__
#include <pthread_np.h> /* pthread w/ affinity */
#include <sys/cpuset.h> /* cpu_set */
#endif /* __FreeBSD__ */

#ifdef linux
#define cpuset_t        cpu_set_t
#endif


#ifdef __APPLE__

#include <mach/mach.h>
#include <pthread.h>

#define cpu_set_t	uint32_t

#define cpuset_t        uint64_t        // XXX
static inline void CPU_ZERO(cpuset_t *p)
{
        *p = 0;
}

static inline void CPU_SET(uint32_t i, cpuset_t *p)
{
        *p |= 1<< (i & 0x3f);
}

static inline int CPU_ISSET(uint32_t i, cpuset_t *p)
{
        return (*p & (1<< (i & 0x3f)) );
}

/*
 * simplified version, we only bind to one core or all cores
 * if the mask contains more than 1 bit
 */
static inline int
pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
                           cpuset_t *cpu_set)
{
  thread_port_t mach_thread;
  int core, lim = 8 * cpusetsize;

  for (core = 0; core < lim; core++) {
    if (CPU_ISSET(core, cpu_set)) break;
  }
  if (core == lim || (*cpu_set & ~(1 << core)) != 0) {
	core = -1;
  }
  printf("binding to core %d 0x%lx\n", core, (u_long)*cpu_set);
  thread_affinity_policy_data_t policy = { core+1 };
  mach_thread = pthread_mach_thread_np(thread);
  thread_policy_set(pthread_mach_thread_np(thread), THREAD_AFFINITY_POLICY,
                    (thread_policy_t)&policy, 1);
  return 0;
}

#define sched_setscheduler(a, b, c)     (1) /* error */

#include <libkern/OSAtomic.h>

#define clock_gettime(a,b)      \
        do {struct timespec t0 = {0,0}; *(b) = t0; } while (0)
#endif /* APPLE */

#include <stdlib.h>	/* strtol */


#define SAFE_CALLOC(_sz)					\
    ({	int sz = _sz; void *p = sz>0 ? calloc(1, sz) : NULL;	\
	if (!p) { D("alloc error %d bytes", sz); exit(1); }	\
	 p;} )


#include <sys/uio.h>
#include "../proxy/backend.h"
struct mbuf {
    struct iovec iov;
    struct BpfhvBackend *be;
    struct BpfhvBackendQueue *txq;
    uint64_t idx;
	uint16_t flow_id;	/* for testing, index of a flow */
#ifndef MY_MQ_LEN
        struct mbuf *m_nextpkt;
#endif
        struct mbuf *m_freelist;	/* XXX testing */
};

struct mbuf_cache {
    struct mbuf *cache;
    struct mbuf *first_free;
};

void *sched_init(int ac, char *av[]);
int  sched_enq(void *, struct mbuf *);
struct mbuf *sched_deq(void *);
uint32_t get_flow_count(void *c);

int dump(void *c);

struct sched_all;

/*
 * structure describing patterns to send:

Base commands are duration > 0 in bits, pkt-size > 0
A size of 0 specifies silence;
a negative duration terminates
A zero duration jumps to the beginning
bpp is the number of bits per packet to be used in the computation.
bpp = pkt_len*8 if the rate is in bps, and 1 if the rate is in pps

 */
struct _txp {
    double duration;
    double rate; /* bits/s */
    uint16_t pkt_len; /* individual packets */
#define TXP_DEFLEN  1500
    uint16_t burst_len; /* packets */
    uint32_t bpp; /* bits per packet in the timing loop. 1 in case of pps */
};

struct sched_td;
struct cqueue_client;
typedef uint32_t (*cli_submit_t)(struct cqueue_client *cq,
                                     struct sched_td *td,
                                     uint32_t burst_len, uint16_t pkt_len);
typedef uint32_t (*cli_xmit_clean_t)(struct cqueue_client *cq,
                                     struct sched_td *td, uint16_t pkt_len);

/*
 * each scheduler has several callbacks
 */
struct sched_template {
    const char *name;
    /* common open ? */
    /* per-thread open ? */
    /* per-thread close ? */
    /* common close ? */
    cli_submit_t submit;
    cli_xmit_clean_t xmit_clean;
};

struct sched_td { /* per thread info */
    struct sched_all *parent;
    pthread_t td_id;

    uint32_t my_id; /* 0.. n_threads - 1 */
    uint32_t ready;

    /* parameters for traffic generation */
    uint64_t task_start;
    uint64_t burst_start;
    uint32_t wl_len; /* entries in worklist */
    uint32_t wl_sz;  /* max entries in worklist */
#define SCHED_MAXSZ   1000
    struct _txp *worklist;

    int sockfd; /* file descriptor for UDP socket to send() */
    void *nmd;  /* netmap descriptor */

    int timestamps; /* 1 if clients need to timestamp packets */
    int active; /* 1 if this client can do real I/O. */
    int use_mmsg; /* Use sendmmsg() */

    cli_submit_t cli_submit; /* callback to send a burst */
    cli_xmit_clean_t cli_xmit_clean; /* callback to clean a burst */

    struct cqueue *q;
    uint32_t qsize;

    uint8_t packet[1500]; /* packet to be sent */

#define NMMSG       64
    //struct mmsghdr mmsg[NMMSG];
    struct iovec onemsg;

    char name[128]; /* debugging */
};

struct sched_args;

struct sched_all {
    uint32_t n_clients;
    uint32_t n_active_threads;

    const char *dst_ip_addr; /* destination ip address */

    uint64_t ticks_per_second;
    struct sched_td *td; /* n_threads entries */

    int stop;
    int need_scan;
    void *sched;
    pthread_t sched_id;
    uint64_t sched_interval; /* nanoseconds */
    uint64_t sched_interval_tsc; /* tsc cycles */
    uint16_t sched_byte_limit;
    uint16_t sched_batch_limit;
    uint64_t client_interval; /* nanoseconds */
    uint64_t client_interval_tsc; /* tsc cycles */
    int sched_affinity;
    double sched_bw;
    double bytes_to_tsc;
    int timestamps;
    int use_mmsg;

    /* copy of sched->flows */
    uint32_t max_mark;

    /* Dequeue function based on chosen backend */
    uint32_t(*sched_deq_f)(struct sched_all *f, uint64_t now);

    /* Scheduler output interface */
    struct nm_desc *nmd;

    int multi_udp_ports;

    /* Structures used by do_sched(). */
    struct cqueue_sched *cqs;
    struct mbuf_cache mbc;

    uint64_t next_link_idle;
	/* when the link becomes idle. This is at the end of
	 * the previous packet, or last time we checked and
	 * found it was idle and no queued packets.
	 */
    uint64_t sched_start;
    uint64_t sched_end;
#define MAXFNAME 256
    const char *stat_prefix;
    uint64_t stat_check_idle;
    uint64_t stat_sched_idle;
    uint64_t stat_batch_full;
    uint64_t stat_early;
    uint64_t n_sch_pub;
    uint64_t n_sch_fetch;
    uint64_t n_sch_released;
    uint64_t n_sch_released_bytes;
};


char ** split_arg(const char *src, int *_ac);
/* conversion factor for numbers.
 * Each entry has a set of characters and conversion factor,
 * the first entry should have an empty string and default factor,
 * the final entry has s = NULL.
 */
struct _sm {    /* string and multiplier */
	char *s;
	double m;
};
uint64_t parse_time(const char *arg);
uint64_t parse_bw(const char *arg);
uint64_t parse_qsize(const char *arg);
#define U_PARSE_ERR ~(0ULL)

void do_work(uint64_t cpu_cycles);

#include <math.h> /* log, exp etc. */
static inline uint64_t
my_random24(void)       /* 24 useful bits */
{
        return random() & ((1<<24) - 1);
}

/*
 * parse a generic value
 */
double parse_gen(const char *arg, const struct _sm *conv, int *err);

int do_socket(const char *addr, int port, int client, int nonblock);
uint32_t safe_write(int fd, const char *buf, uint32_t l);

struct cqueue_sched {
    uint64_t sch_extract_next; /* tsc value for next extract retry */
};

#include "tsc.h"

// int safe_read(int fd, const char *buf, int l);

#endif /* __SCHED16_H__ */
