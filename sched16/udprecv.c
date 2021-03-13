#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <net/if.h>

#define NETMAP_WITH_LIBS
#include <libnetmap.h>

#include "tsc.h"

#ifdef __APPLE__

#define cpuset_t        uint64_t        // XXX
inline void CPU_ZERO(cpuset_t *p)
{
        *p = 0;
}

inline void CPU_SET(uint32_t i, cpuset_t *p)
{
        *p |= 1<< (i & 0x3f);
}

#define pthread_setaffinity_np(a, b, c) ((void)a, 0)

#define ifr_flagshigh  ifr_flags        // XXX
#define IFF_PPROMISC   IFF_PROMISC
#include <net/if_dl.h>  /* LLADDR */

#define clock_gettime(a, b) \
	do {struct timespec t0 = {0,0}; *(b) = t0; } while (0)

#endif  /* __APPLE__ */

/* Global variables to hold latency stats. They can be used
 * by one client only. */
#define N_LATENCY_STAT    30000
static uint64_t lat_stat[N_LATENCY_STAT];
static int lat_stat_idx = 0;

#define BUFLEN 512

struct global;

struct tpriv {
    pthread_t           tid;
    unsigned int        myid;
    unsigned int        timestamps;
    int                 sfd;
    struct global       *g;
};

struct global {
    struct tpriv        *th;
    const char          *ipstr;
    unsigned int        port;
    unsigned int        core_id_base;
    unsigned int        blocking;
    const char          *sfname;
    unsigned int        num_active_threads;
    unsigned int        num_threads;
    unsigned int        cdf;  /* cumulative density function */
    const char          *netmapif;
};

static struct global _g;

static void
quit(char *s)
{
    perror(s);
    exit(1);
}

/* id of the client to filter for latency measurements */
#define FLT_ID  0

static void *
do_receive_socket(void *opaque)
{
    struct tpriv *priv = opaque;
    struct sockaddr_in srv_addr;
    unsigned long long n, n_flt, limit;
    struct timespec t_last;
    double latency = 0;
    char buf[BUFLEN];

    priv->sfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (priv->sfd < 0) {
        quit("socket");
    }

    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(priv->g->port + priv->myid);

    if (inet_aton(priv->g->ipstr, &srv_addr.sin_addr) == 0) {
        quit("inet_aton() failed");
    }

    if (bind(priv->sfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == -1) {
        quit("bind");
    }

    if (priv->myid >= priv->g->num_active_threads) {
        printf("receiver %u will be inactive\n", priv->myid);
        return NULL;
    }

    if (!priv->g->blocking) {
        printf("setting non-blocking mode for fd %d\n", priv->sfd);
        if (fcntl(priv->sfd, F_SETFL, O_NONBLOCK)) {
            quit("failed to set O_NONBLOCK");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_last);
    limit = 100ULL;

    if (priv->g->core_id_base != ~0U) {
        runon("udprecv", priv->g->core_id_base + priv->myid);
    } else {
        printf("udprecv thread not pinned to a specific core\n");
    }

    for (n=0, n_flt = 0;;) {
        int ret = recvfrom(priv->sfd, buf, BUFLEN, 0, NULL, NULL);

        if (ret == -1) {
            if (errno == EAGAIN) {
                continue;
            }
            quit("recvfrom()");
        }

        if (priv->timestamps && ret >= 2 * (int)sizeof(uint64_t)) {
            uint64_t *tsp = (uint64_t *)buf;

            if (*tsp == FLT_ID) {
                uint64_t x;
                tsp ++;
                x = rdtsc() - *tsp;
                lat_stat[lat_stat_idx++] = x;
                if (lat_stat_idx == N_LATENCY_STAT) {
                    lat_stat_idx = 0;
                }

                if ((double)x > latency) {
                    latency = x;
                } else {
                    latency = latency * 0.99 + x * 0.01;
                }
                n_flt ++;
            }
        }

        if (n >= limit) {
            unsigned long long diff_ns;
            struct timespec t_cur;
            double rate;
            double grate;

            clock_gettime(CLOCK_MONOTONIC, &t_cur);
            diff_ns = (t_cur.tv_sec - t_last.tv_sec) * 1e9 +
                      (t_cur.tv_nsec - t_last.tv_nsec);

            if (diff_ns < 3e9) {
                limit <<= 1;
            } else if (limit > 1 && diff_ns > 5e9) {
                limit >>= 1;
            }

            rate = (((double)n) * 1e3) / diff_ns;
            grate = (((double)n_flt) * 1e6) / diff_ns;
            printf("#%02d Receiving %5.6f Mpps [good %5.3f Kpps], latency %llu ns\n",
                   priv->myid, rate, grate, (long long unsigned)
                   (TSC2NS(latency)));

            n = 0;
            n_flt = 0;
            clock_gettime(CLOCK_MONOTONIC, &t_last);
        }

        n ++;
    }

    close(priv->sfd);

    return NULL;
}

#define UDP_PLD_OFS     (14 + 20 + 8)

static void *
do_receive_netmap(void *opaque)
{
    struct tpriv *priv = opaque;
    unsigned long long n, n_flt, limit;
    struct netmap_ring *ring;
    struct timespec t_last;
    struct nm_desc *nmd;
    double latency = 0;
    int ni = 0;
    uint32_t h;
    int r;

    nmd = nm_open(priv->g->netmapif, NULL, 0, NULL);
    if (nmd == NULL) {
        quit("nm_open()");
    }

    if (!priv->g->blocking) {
        printf("setting non-blocking mode for fd %d\n", nmd->fd);
        if (fcntl(nmd->fd, F_SETFL, O_NONBLOCK)) {
            quit("failed to set O_NONBLOCK");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_last);
    limit = 100ULL;

    if (priv->g->core_id_base != ~0U) {
        runon("udprecv", priv->g->core_id_base + priv->myid);
    } else {
        printf("udprecv thread not pinned to a specific core\n");
    }

    for (n=0, n_flt = 0;;) {
        uint64_t base_ts;

        if (ioctl(nmd->fd, NIOCRXSYNC, NULL)) {
            D("ioctl() failed");
            break;
        }

        ni ++;

        base_ts = rdtsc();

        for (r = nmd->first_rx_ring; r <= nmd->last_rx_ring; r++) {
            ring = NETMAP_RXRING(nmd->nifp, r);
            while (!nm_ring_empty(ring)) {
                h = ring->head;
                if (ring->slot[h].len >= UDP_PLD_OFS + 2 * (int)sizeof(uint64_t)) {
                    uint64_t *tsp = (uint64_t *)(NETMAP_BUF(ring, ring->slot[h].buf_idx) + UDP_PLD_OFS);

                    if (*tsp == FLT_ID) {
                        uint64_t x;
                        tsp ++;
                        x = base_ts - *tsp;
                        lat_stat[lat_stat_idx++] = x;
                        if (lat_stat_idx == N_LATENCY_STAT) {
                            lat_stat_idx = 0;
                        }

                        if ((double)x > latency) {
                            latency = x;
                        } else {
                            latency = latency * 0.99 + x * 0.01;
                        }
                        n_flt ++;
                    }
                }
                n++;

                ring->cur = ring->head = nm_ring_next(ring, h);
            }
        }

        if (n >= limit) {
            unsigned long long diff_ns;
            struct timespec t_cur;
            double rate;
            double grate;
            double bsize;

            clock_gettime(CLOCK_MONOTONIC, &t_cur);
            diff_ns = (t_cur.tv_sec - t_last.tv_sec) * 1e9 +
                      (t_cur.tv_nsec - t_last.tv_nsec);

            if (diff_ns < 3e9) {
                limit <<= 1;
            } else if (limit > 1 && diff_ns > 5e9) {
                limit >>= 1;
            }

            rate = (((double)n) * 1e3) / diff_ns;
            grate = (((double)n_flt) * 1e6) / diff_ns;
            if (ni)
                bsize = (double)n/ni;
            printf("#%02d Receiving %5.6f Mpps [good %5.3f Kpps], latency %llu ns batchsize %f\n",
                   priv->myid, rate, grate, (long long unsigned)
                   (TSC2NS(latency)), bsize);

            n = 0;
            n_flt = 0;
            ni = 0;
            clock_gettime(CLOCK_MONOTONIC, &t_last);
        }
    }

    nm_close(nmd);

    return NULL;
}

static void
quick_sort(int64_t *arr, int elements)
{
#define  MAX_LEVELS  300

    int64_t piv, beg[MAX_LEVELS], end[MAX_LEVELS],
    i = 0, L, R, swap ;

    beg[0] = 0; end[0] = elements;
    while (i >= 0) {
        L = beg[i];
        R = end[i] - 1;
        if (L < R) {
            piv = arr[L];
            while (L < R) {
                while (arr[R] >= piv && L < R) {
                    R--;
                }
                if (L < R) {
                    arr[L++] = arr[R];
                }
                while (arr[L] <= piv && L < R) {
                    L++;
                }
                if (L < R)
                    arr[R--] = arr[L];
            }
            arr[L] = piv;
            beg[i + 1] = L + 1;
            end[i + 1] = end[i];
            end[i++] = L;
            if (end[i] - beg[i] > end[i-1] - beg[i-1]) {
                swap = beg[i];
                beg[i] = beg[i - 1];
                beg[i - 1] = swap;
                swap = end[i];
                end[i] = end[i - 1];
                end[i - 1] = swap;
            }
        } else {
            i--;
        }
    }
}

static void
sigint_handler(int signo)
{
    struct global *g = &_g;
    uint64_t lat_sum = 0;
    FILE *sout;
    unsigned int i;

    (void)signo;

    for (i = 0; i < N_LATENCY_STAT; i++) {
        lat_stat[i] = TSC2NS(lat_stat[i]);
        lat_sum += lat_stat[i];
    }

    quick_sort((int64_t *)lat_stat, N_LATENCY_STAT);

    printf("Xth percentile = %llu\n", (long long unsigned)
            lat_stat[(N_LATENCY_STAT * 95) / 100]);

    if (!g->sfname) {
        exit(0);
    }

    sout = fopen(g->sfname, "w");
    if (!sout) {
        perror("Failed to open output file for statistics");
        exit(0);
        return;
    }

    for (i = 0; i < N_LATENCY_STAT; i++) {
        if (g->cdf) {
            if (i == N_LATENCY_STAT - 1 || lat_stat[i+1] != lat_stat[i]) {
                fprintf(sout, "%llu %3.6f\n", (long long unsigned)lat_stat[i],
                                              ((float)i)/N_LATENCY_STAT);
            }
        } else {
            if (lat_stat[i] > 0) {
                fprintf(sout, "%llu\n", (long long unsigned)lat_stat[i]);
            }
        }
    }

    fclose(sout);

    exit(0);
}

static void
usage(void)
{
    fprintf(stderr, "./udprecv [-h] [-p first UDP port to listen on] "
                    "[-a IP address to listen on] "
                    "[-n number of receiving threads] "
                    "[-b <use blocking sockets>] "
                    "[-T <read timestamps from packets and compute average latency>] "
                    "[-c first core id to use] "
                    "[-o file to output latency distribution or CDF] "
                    "[-m number of threads which will actively recvmsg()] "
                    "[-C <compute and output cumulative density function>] "
                    "[-N netmap interface to listen on] "
                    "\n");
}

int main(int argc, char **argv)
{
    void * (*receive_handler)(void *);
    unsigned int timestamps = 0;
    struct global *g = &_g;
    int m_arg_set = 0;
    unsigned int i;
    int ch;

    calibrate_tsc();

    g->port = 9302;
    g->ipstr = "0.0.0.0";
    g->blocking = 0;
    g->core_id_base = ~0U;
    g->sfname = NULL;
    g->cdf = 0;
    g->num_threads = 1;
    g->netmapif = NULL;

    while ((ch = getopt(argc, argv, "hbp:a:n:m:c:To:CN:")) != -1) {
	switch (ch) {
        case 'h':
        default:
            usage();
            exit(0);
            break;

        case 'p': /* UDP port number to listen on */
            g->port = atoi(optarg);
            break;

        case 'a': /* IP address to listen on */
            g->ipstr = optarg;
            break;

        case 'n': /* number of threads */
            g->num_threads = atoi(optarg);
            break;

        case 'm': /* number of active receivers, must be <=n */
            g->num_active_threads = atoi(optarg);
            m_arg_set = 1;
            break;

        case 'b': /* use blocking mode for UDP sockets */
            g->blocking = 1;
            break;

        case 'c': /* first core id to use for receive threads */
            g->core_id_base = atoi(optarg);
            break;

        case 'T':
            timestamps = 1;
            break;

        case 'o':
            g->sfname = optarg;
            break;

        case 'C':
            g->cdf = 1;
            break;

        case 'N': /* use netmap interface instead of UDP socket */
            g->netmapif = optarg;
            break;
        }
    }

    if (g->netmapif) { /* Only one thread is necessary in netmap mode. */
        receive_handler = do_receive_netmap;
        g->num_threads = 1;
    } else {
        receive_handler = do_receive_socket;
    }

    if (!m_arg_set) {
        g->num_active_threads = g->num_threads;
    }

    if (g->num_active_threads > g->num_threads) {
        g->num_active_threads = g->num_threads;
    }

    signal(SIGINT, sigint_handler);

    g->th = calloc(g->num_threads, sizeof(*g->th));
    if (!g->th) {
        printf("Out of memory\n");
        exit(1);
    }

    for (i = 0; i < g->num_threads; i++) {
        g->th[i].g = g;
        g->th[i].myid = i;
        g->th[i].timestamps = timestamps;
        pthread_create(&g->th[i].tid, NULL, receive_handler, g->th + i);
    }

    for (i = 0; i < g->num_threads; i++) {
        pthread_join(g->th[i].tid, NULL);
    }

    if (g->num_threads > 0 && g->num_active_threads == 0) {
        while (1) {
            sleep(5);
        }
    }

    free(g->th);

    return 0;
 }
