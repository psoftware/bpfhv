#include "bpfhv.h"
#include "sring.h"
#include "net_headers.h"

#ifndef __section
# define __section(NAME)                  \
   __attribute__((section(NAME), used))
#endif

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
    const uint32_t STREAM_1 = 10;
    const uint32_t STREAM_2 = 11;
    const uint32_t DEFAULT_CLASS = 99; /* = priv->queue_n; last scheduler queue */
    /* rule list */
    if(iph->protocol == IPPROTO_ICMP && iph->tos == 0x50)
        return STREAM_1;
    if(iph->protocol == IPPROTO_ICMP && iph->tos == 0x51)
        return STREAM_2;

    return DEFAULT_CLASS;
}

__section("txp")
int sring_txp(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    struct bpfhv_buf *txb = ctx->bufs + 0;
    uint32_t prod = priv->prod;
    struct sring_tx_desc *txd;

    if (ctx->num_bufs != 1) {
        return -1;
    }

    txd = priv->desc + (prod & priv->qmask);
    txd->cookie = txb->cookie;
    txd->paddr = txb->paddr;
    txd->len = txb->len;
    txd->mark = mark_packet(ctx);

    /* Make sure stores to sring entries happen before store to priv->prod. */
    smp_mb_release();
    ACCESS_ONCE(priv->prod) = prod + 1;
    /* Full memory barrier to make sure store to priv->prod happens
     * before load from priv->kick_enabled (see corresponding double-check
     * in the hypervisor/backend TXQ drain routine). */
    smp_mb_full();
    ctx->oflags = ACCESS_ONCE(priv->kick_enabled) ?
                  BPFHV_OFLAGS_KICK_NEEDED : 0;

    return 0;
}

static inline void
sring_tx_get_one(struct bpfhv_tx_context *ctx,
                 struct sring_tx_context *priv)
{
    struct bpfhv_buf *txb = ctx->bufs + 0;
    struct sring_tx_desc *txd;

    txd = priv->desc + (priv->clear & priv->qmask);
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

    sring_tx_get_one(ctx, priv);
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
