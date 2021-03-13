/*
 * BSD license
 */

/*
 * Session handler to run and network communication
 * over a TCP socket, and also run the callbacks.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h> // inet_aton
#include <netinet/in.h>
#include <netinet/tcp.h>	// TCP_NODELAY
//#include <sys/cpuset.h> // freebsd, used in rmlock
#include <sys/errno.h>
extern int errno;

#include "sched16.h"
#include "tsc.h"

#include <stdio.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>	/* timersub */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>	/* read() */

#define SOCK_QLEN 5     /* listen lenght for incoming connection */

/* waste some time spinning around rdtsc */
void
do_work(uint64_t cpu_cycles)
{
    if (cpu_cycles) {
	uint64_t start_tsc = rdtsc();
	while (rdtsc() - start_tsc < cpu_cycles)
	    ;
    }
}

uint32_t
safe_write(int fd, const char *buf, uint32_t l)
{
	uint32_t i = 0;
	int n = 0;
	for (i = 0; i < l; i += n) {
		n = write(fd, buf + i, l - i);
		if (n <= 0) {
			D("short write");
			break;
		}
	}
	ND(1,"done, i %d l %d n %d", i, l, n);
	return i;
}

/*
 * listen on a socket,
 * return the listen fd or -1 on error.
 */
int
do_socket(const char *addr, int port, int client, int nonblock)
{
	int fd = -1, on, ret;
	struct sockaddr_in s;

	/* open the listen socket */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror( "socket" );
		return -1;
	}
	if (nonblock)
		fcntl(fd, F_SETFL, O_NONBLOCK);

	on = 1;
#ifdef SO_REUSEADDR
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
		perror("SO_REUSEADDR failed(non fatal)");
#endif
#ifdef SO_REUSEPORT
	on = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) == -1)
		perror("SO_REUSEPORT failed(non fatal)");
#endif

	/* fill the sockaddr struct */
	bzero(&s, sizeof(s));
        s.sin_family = AF_INET;
        inet_aton(addr, &s.sin_addr);
        s.sin_port = htons(port);

	if (client) {
        	ret = connect(fd, (struct sockaddr*) &s, sizeof(s));
		if (ret < 0 && ret != EINPROGRESS) {
			D("connect error");
			return -1;
		}
		D("+++ connected to tcp %s:%d",
		    inet_ntoa(s.sin_addr), ntohs(s.sin_port));

	} else {
        	ret = bind(fd, (struct sockaddr*) &s, sizeof(s));
		if (ret < 0) {
			perror( "bind" );
			return -1;
		};
		D("+++ listening tcp %s:%d",
		    inet_ntoa(s.sin_addr), ntohs(s.sin_port));

		/* listen for incoming connection */
		ret = listen(fd, SOCK_QLEN);
		if (ret < 0) {
			perror("listen");
			return -1;
		}
		D("listen returns %d", ret);
	}
	return fd;
}

/* number parsing */

/*---- configuration handling ---- */
/*
 * support routine: split argument, returns ac and *av.
 * av contains two extra entries, a NULL and a pointer
 * to the entire string.
 */
char **
split_arg(const char *src, int *_ac)
{
    char *my = NULL, **av = NULL, *seps = " \t\r\n,";
    int l, i, ac; /* number of entries */

    if (!src)
	return NULL;
    l = strlen(src);
    /* in the first pass we count fields, in the second pass
     * we allocate the av[] array and a copy of the string
     * and fill av[]. av[ac] = NULL, av[ac+1] 
     */
    for (;;) {
	i = ac = 0;
	ND("start pass %d: <%s>", av ? 1 : 0, my);
	while (i < l) {
	    /* trim leading separator */
	    while (i <l && strchr(seps, src[i]))
		i++;
	    if (i >= l)
		break;
	    ND("   pass %d arg %d: <%s>", av ? 1 : 0, ac, src+i);
	    if (av) /* in the second pass, set the result */
		av[ac] = my+i;
	    ac++;
	    /* skip string */
	    while (i <l && !strchr(seps, src[i])) i++;
	    if (av)
		my[i] = '\0'; /* write marker */
	}
	ND("ac is %d", ac);
	if (!av) { /* end of first pass */
	    av = calloc(1, (l+1) + (ac + 2)*sizeof(char *));
            if (!av) {
                D("calloc() failed");
                exit(EXIT_FAILURE);
            }
	    my = (char *)&(av[ac+2]);
	    strcpy(my, src);
	} else {
	    break;
	}
    }
    av[i++] = NULL;
    av[i++] = my;
    *_ac = ac;
    return av;
}

/*
 * parse a generic value
 */
double
parse_gen(const char *arg, const struct _sm *conv, int *err)
{
	double d;
	char *ep;
	int dummy;

	if (err == NULL)
		err = &dummy;
	*err = 0;
	if (arg == NULL)
		goto error;
	d = strtod(arg, &ep);
	if (ep == arg) { /* no value */
		D("bad argument %s", arg);
		goto error;
	}
	/* special case, no conversion */
	if (conv == NULL && *ep == '\0')
		goto done;
	ND("checking %s [%s]", arg, ep);
	for (;conv->s; conv++) {
		if (strchr(conv->s, *ep))
			goto done;
	}
error:
	*err = 1;	/* unrecognised */
	return 0;

done:
	if (conv) {
		ND("scale is %s %lf", conv->s, conv->m);
		d *= conv->m; /* apply default conversion */
	}
	ND("returning %lf", d);
	return d;
}

/* returns a value in nanoseconds */
uint64_t
parse_time(const char *arg)
{
    struct _sm a[] = {
	{"", 1000000000 /* seconds */},
	{"n", 1 /* nanoseconds */}, {"u", 1000 /* microseconds */},
	{"m", 1000000 /* milliseconds */}, {"s", 1000000000 /* seconds */},
	{NULL, 0 /* seconds */}
    };
    int err;
    uint64_t ret = (uint64_t)parse_gen(arg, a, &err);
    return err ? U_PARSE_ERR : ret;
}

/*
 * parse a bandwidth, returns value in bps or U_PARSE_ERR if error.
 */
uint64_t
parse_bw(const char *arg)
{
    struct _sm a[] = {
	{"", 1}, {"kK", 1000}, {"mM", 1000000}, {"gG", 1000000000}, {NULL, 0}
    };
    int err;
    uint64_t ret = (uint64_t)parse_gen(arg, a, &err);
    return err ? U_PARSE_ERR : ret;
}

/*
 * parse a queue size, returns value in bytes or U_PARSE_ERR if error.
 */
uint64_t
parse_qsize(const char *arg)
{
    struct _sm a[] = {
	{"", 1}, {"kK", 1024}, {"mM", 1024*1024}, {"gG", 1024*1024*1024}, {NULL, 0}
    };
    int err;
    uint64_t ret = (uint64_t)parse_gen(arg, a, &err);
    return err ? U_PARSE_ERR : ret;
}

#if 0
/*
 * For some function we need random bits.
 * This is a wrapper to whatever function you want that returns
 * 24 useful random bits.
 */

#include <math.h> /* log, exp etc. */
static inline uint64_t
my_random24(void)	/* 24 useful bits */
{
	return random() & ((1<<24) - 1);
}

#endif
