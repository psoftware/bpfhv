#include "bpfhv.h"
#include "sring.h"
#include "net_headers.h"

#ifndef __section
# define __section(NAME)                  \
   __attribute__((section(NAME), used))
#endif

static int BPFHV_FUNC(print_num, char c, long long int num);
static int BPFHV_FUNC(rx_pkt_alloc, struct bpfhv_rx_context *ctx);
static void* BPFHV_FUNC(pkt_network_header, struct bpfhv_tx_context *ctx);
static void* BPFHV_FUNC(pkt_transport_header, struct bpfhv_tx_context *ctx);
static int BPFHV_FUNC(smp_mb_full);


#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))
#define compiler_barrier() __asm__ __volatile__ ("");
#define smp_mb_release()    compiler_barrier()
#define smp_mb_acquire()    compiler_barrier()

static inline int
clear_met_cons(uint32_t old_clear, uint32_t cons, uint32_t new_clear)
{
    return (uint32_t)(new_clear - cons - 1) < (uint32_t)(new_clear - old_clear);
}

uint32_t mark_packet(struct bpfhv_tx_context *ctx) {
    /* 1) extract data from packet for marking */
    struct iphdr *iph = (struct iphdr *)pkt_network_header(ctx);

    void *transport_header = pkt_transport_header(ctx);
    struct udphdr *udp_header;
    struct tcphdr *tcp_header;
    uint32_t src_ip = (unsigned int)iph->saddr;
    uint32_t dest_ip = (unsigned int)iph->daddr;
    uint16_t src_port = 0;
    uint16_t dest_port = 0;

    if (iph->protocol == IPPROTO_UDP) {
        udp_header = (struct udphdr *)transport_header;
        src_port = (unsigned int)be16_to_cpu(udp_header->source);
    } else if (iph->protocol == IPPROTO_TCP) {
        tcp_header = (struct tcphdr *)transport_header;
        src_port = (unsigned int)be16_to_cpu(tcp_header->source);
        dest_port = (unsigned int)be16_to_cpu(tcp_header->dest);
    }

    /* 2) mark packets */
    const uint32_t RESERVED = 0;
    const uint32_t STREAM_1 = 1;
    const uint32_t STREAM_2 = 2;
    const uint32_t DEFAULT_CLASS = 3; /* = priv->queue_n; last scheduler queue */
    /* rule list */
    if(iph->protocol == IPPROTO_TCP && dest_port == 30000)
        return STREAM_1;
    if(iph->protocol == IPPROTO_TCP && dest_port == 30001)
        return STREAM_2;

    return DEFAULT_CLASS;
}

/* Mark packet and drop if there is no space in the related subqueue
   We drop it here because txp routine does not allow to drop packets */
__section("txh")
int sring_txh(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    uint32_t prod = priv->prod;
    uint32_t clear = priv->clear;

    /* 1) check if transmit queue has space, in that case we can directly transmit */
    uint32_t queued = (prod >= clear) ? (prod - clear) : (priv->queue_buffs - clear + prod);

    if(queued < priv->queue_buffs) {
        //print_num("txh: accept for transmit queue", 0);
        priv->mark = 0; /* mark as direct transmission to transmit queue */
        return 0;
    }

    /* 2) mark packet */
    uint32_t mark = mark_packet(ctx);

    /* 3) check if class queue is full and drop if it's the case */
    struct sring_tx_schqueue_context *scq = sring_tx_context_subqueue(priv, mark);
    if (scq->used >= priv->queue_buffs) {
        //print_num("txh: drop, class queue is full", 1);
        return -1; // drop
    }

    priv->mark = mark;

    //print_num("txh: accept for class queue", 2);
    return 0;
}

__section("txp")
int sring_txp(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    struct bpfhv_buf *txb = ctx->bufs + 0;
    uint32_t prod = priv->prod;
    struct sring_tx_desc *txd;
    uint32_t queue_class;
    struct sring_tx_schqueue_context *scq,*txq_main;

    if (ctx->num_bufs != 1) {
        return -1;
    }

    txq_main = sring_tx_context_subqueue(priv, 0);
    queue_class = priv->mark;

    // 1) check if there is space to add packets. if not, add to class queue
    /*queued = (prod >= clear) ? (prod - clear) : (priv->queue_buffs - clear + prod);
    queue_class = (queued >= priv->queue_buffs) ? mark : 0; */
    scq = sring_tx_context_subqueue(priv, queue_class);
    prod = (queue_class == 0) ? prod : scq->prod;

    txd = scq->desc + (prod & priv->qmask);
    txd->cookie = txb->cookie;
    txd->paddr = txb->paddr;
    txd->len = txb->len;

    /* packet goes in a scheduler queue */
    if(queue_class != 0) {
        /* queue cannot be full because it is checked in sring_txh routine */
        scq->used++;
        scq->prod++;
        priv->total_queued_buffs++;
        // we don't need memory barriers because subqueues are guest only
        // (publish and complete are synchronous)
        //print_num("scheduled to queue", 10);
    } else {
        //print_num("not scheduled to queue", 11);

        /* packet goes in transmit queue */
        /* Make sure stores to sring entries happen before store to priv->prod. */
        smp_mb_release();
        ACCESS_ONCE(priv->prod) = prod + 1;
        /* Full memory barrier to make sure store to priv->prod happens
         * before load from priv->kick_enabled (see corresponding double-check
         * in the hypervisor/backend TXQ drain routine). */
        smp_mb_full();
    }
    ctx->oflags = ACCESS_ONCE(priv->kick_enabled) ?
                  BPFHV_OFLAGS_KICK_NEEDED : 0;

    return 0;
}

static int
sring_scheduler_dequeue_one(struct sring_tx_context *priv) {
    struct sring_tx_schqueue_context *scq;
    struct sring_tx_schqueue_context *txq_main = sring_tx_context_subqueue(priv, 0);
    struct sring_tx_desc *scq_head;
    struct sring_tx_desc *txd_prod;
    uint32_t current_queue = priv->current_queue;

    /* nothing to schedule */
    if(priv->total_queued_buffs == 0)
        return 0;

    /* resume dequeuing starting from last queue and deficit */
    for(uint32_t i = current_queue; /* no loop bound? */; i = (i+1) % priv->queue_n) {
        scq = sring_tx_context_subqueue(priv, i+1);
        if(scq->used != 0) {
            /* don't add deficit if last time there were more packets to pick from current_queue */
            scq->deficit += scq->quantum*scq->weight*priv->add_deficit;
            scq_head = scq->desc + (scq->cons & priv->qmask);

            //while(scq->used != 0 && scq->deficit >= scq_head->len) {
            if(scq->deficit >= scq_head->len) {
                scq->deficit -= scq_head->len;

                /* Copy head packet descriptor to main queue head packet descriptor */
                txd_prod = txq_main->desc + (priv->prod & priv->qmask);
                txd_prod->cookie = scq_head->cookie;
                txd_prod->paddr = scq_head->paddr;
                txd_prod->len = scq_head->len;

                /* Make sure stores to sring entries happen before store to priv->prod. */
                smp_mb_release();
                ACCESS_ONCE(priv->prod) = priv->prod + 1;
                /* Full memory barrier to make sure store to priv->prod happens
                 * before load from priv->kick_enabled (see corresponding double-check
                 * in the hypervisor/backend TXQ drain routine). */
                smp_mb_full();

                scq->cons++;
                scq->used--;
                priv->total_queued_buffs--;

                /* next dequeuing should start from current queue if not empty, otherwise
                 * we should pick next queue */
                scq_head = scq->desc + (scq->cons & priv->qmask);
                priv->add_deficit = (scq->used != 0 && scq->deficit >= scq_head->len) ? 0 : 1;
                priv->current_queue = (priv->add_deficit) ? ((i+1) % priv->queue_n) : i;

                /* deficit is cumulated only if list is not empty */
                if(scq->used == 0)
                    scq->deficit = 0;

                return 1;
            }
        }

        /* if moving to next queue, than deficit should be added */
        priv->add_deficit = 1;
    }

    /* we should never get here */
    print_num('%', 99);
    return 0;
}

static inline void
sring_tx_get_one(struct bpfhv_tx_context *ctx,
                 struct sring_tx_context *priv)
{
    struct bpfhv_buf *txb = ctx->bufs + 0;
    struct sring_tx_desc *txd;

    txd = sring_tx_context_subqueue(priv, 0)->desc + (priv->clear & priv->qmask);
    txb->paddr = txd->paddr;
    txb->len = txd->len;
    txb->cookie = txd->cookie;
    ctx->num_bufs = 1;
    priv->clear++;
}

__section("txc")
int sring_txc(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    uint32_t cons = ACCESS_ONCE(priv->cons);

    if (priv->clear == cons) {
        return 0;
    }
    /* Make sure load from priv->cons happen before load from sring
     * entries in sring_tx_get_one(). */
    smp_mb_acquire();

    /* Clear one packet through context */
    sring_tx_get_one(ctx, priv);

    /* Schedule one packet to transmit from scheduler queues */
    sring_scheduler_dequeue_one(priv);
    ctx->oflags = 0;

    return 1;
}

__section("txr")
int sring_txr(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    uint32_t cons = ACCESS_ONCE(priv->cons);
    uint32_t clear = priv->clear;
    uint32_t prod = priv->prod;
    int cons_met = (clear == cons);

    if (clear == prod) {
        return 0;
    }
    smp_mb_acquire();
    //TODO: should remove packets
    sring_tx_get_one(ctx, priv);
    if (cons_met) {
        ACCESS_ONCE(priv->cons) = priv->clear;
    }
    ctx->oflags = 0;

    return 1;
}

__section("txi")
int sring_txi(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    uint32_t ncompl;
    uint32_t cons;

    if (ctx->min_completed_bufs == 0) {
        return 0;
    }

    /* min_completed_bufs must be capped to half transmit queue size
     * because num_tx_bufs = #transmit_buffs + N*#subqueue bufs */
    if(ctx->min_completed_bufs > priv->queue_buffs/2)
        ctx->min_completed_bufs = priv->queue_buffs/2;

    cons = ACCESS_ONCE(priv->cons);
    ncompl = cons - priv->clear;

    if (ncompl >= ctx->min_completed_bufs) {
        return 1;
    }
    ACCESS_ONCE(priv->intr_at) = priv->clear + ctx->min_completed_bufs - 1;
    /* Make sure store to priv->intr_at is visible before we
     * load again from priv->cons. */
    smp_mb_full();
    ncompl += ACCESS_ONCE(priv->cons) - cons;
    if (ncompl >= ctx->min_completed_bufs) {
        return 1;
    }

    return 0;
}

__section("rxp")
int sring_rxp(struct bpfhv_rx_context *ctx)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;
    uint32_t prod = priv->prod;
    struct sring_rx_desc *rxd;
    uint32_t i;

    if (ctx->num_bufs > BPFHV_MAX_RX_BUFS) {
        return -1;
    }

    for (i = 0; i < ctx->num_bufs; i++, prod++) {
        struct bpfhv_buf *rxb = ctx->bufs + i;

        rxd = priv->desc + (prod & priv->qmask);
        rxd->cookie = rxb->cookie;
        rxd->paddr = rxb->paddr;
        rxd->len = rxb->len;
    }

    /* Make sure stores to sring entries happen before store to priv->prod. */
    smp_mb_release();
    ACCESS_ONCE(priv->prod) = prod;
    /* Full memory barrier to make sure store to priv->prod happens
     * before load from priv->kick_enabled (see corresponding double-check
     * in the hypervisor/backend RXQ processing routine). */
    smp_mb_full();
    ctx->oflags = ACCESS_ONCE(priv->kick_enabled) ?
                  BPFHV_OFLAGS_KICK_NEEDED : 0;

    return 0;
}

__section("rxc")
int sring_rxc(struct bpfhv_rx_context *ctx)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;
    uint32_t clear = priv->clear;
    uint32_t cons = ACCESS_ONCE(priv->cons);
    struct bpfhv_buf *rxb = ctx->bufs + 0;
    struct sring_rx_desc *rxd;
    int ret;

    if (clear == cons) {
        return 0;
    }
    /* Make sure load from priv->cons happen before load from sring
     * entries. */
    smp_mb_acquire();

    /* Prepare the input arguments for rx_pkt_alloc(). */
    rxd = priv->desc + (clear & priv->qmask);
    rxb->cookie = rxd->cookie;
    rxb->paddr = rxd->paddr;
    rxb->len = rxd->len;
    priv->clear = clear + 1;
    ctx->num_bufs = 1;

    ret = rx_pkt_alloc(ctx);
    if (ret < 0) {
        return ret;
    }

    /* Now ctx->packet contains the allocated OS packet. Return 1 to tell
     * the driver that ctx->packet is valid. Also set ctx->oflags to tell
     * the driver whether rescheduling is necessary. */
    ctx->oflags = 0;

    return 1;
}

__section("rxr")
int sring_rxr(struct bpfhv_rx_context *ctx)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;
    uint32_t cons = ACCESS_ONCE(priv->cons);
    uint32_t clear = priv->clear;
    uint32_t prod = priv->prod;
    uint32_t i = 0;

    if (clear == prod) {
        return 0;
    }
    smp_mb_acquire();

    for (; clear != prod && i < BPFHV_MAX_RX_BUFS; i++) {
        struct bpfhv_buf *rxb = ctx->bufs + i;
        struct sring_rx_desc *rxd;

        rxd = priv->desc + (clear & priv->qmask);
        clear++;
        rxb->cookie = rxd->cookie;
        rxb->paddr = rxd->paddr;
        rxb->len = rxd->len;
    }

    if (clear_met_cons(clear, cons, priv->clear)) {
        ACCESS_ONCE(priv->cons) = clear;
    }
    priv->clear = clear;
    ctx->num_bufs = i;
    ctx->oflags = 0;

    return 1;
}

__section("rxi")
int sring_rxi(struct bpfhv_rx_context *ctx)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;
    uint32_t ncompl;
    uint32_t cons;

    cons = ACCESS_ONCE(priv->cons);
    ncompl = cons - priv->clear;

    if (ncompl >= ctx->min_completed_bufs) {
        ACCESS_ONCE(priv->intr_enabled) = 0;
        return 1;
    }
    ACCESS_ONCE(priv->intr_enabled) = 1;
    /* Make sure store to priv->intr_enabled is visible before we
     * load again from priv->cons. */
    smp_mb_full();
    ncompl += ACCESS_ONCE(priv->cons) - cons;
    if (ncompl >= ctx->min_completed_bufs) {
        ACCESS_ONCE(priv->intr_enabled) = 0;
        return 1;
    }

    return 0;
}
