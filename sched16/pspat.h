#ifndef _PSPATH
#define _PSPATH

#include "sched16.h"

/* Scheduler instance management */
struct sched_all *sched_all_create(int ac, char *av[]);
void sched_all_start(struct sched_all *f, uint32_t num_mbuf);
void sched_all_finish(struct sched_all *f);

uint32_t
fun_sched_enqueue(struct BpfhvBackendProcess *bp, struct BpfhvBackend *be, BpfhvBackendQueue *txq,
                             struct iovec iov, uint64_t opaque_idx, uint32_t mark);

void sched_idle_sleep(struct sched_all *f, uint64_t now, uint32_t ndeq);
uint32_t sched_dequeue(struct sched_all *f, uint64_t now);

#endif