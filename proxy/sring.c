#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/uio.h>

#include "backend.h"
#include "sring.h"

#define MY_CACHELINE_SIZE   64
#define SCHED_TX_QUEUE_N 3
#define TOTAL_TX_QUEUE_N (1 + SCHED_TX_QUEUE_N)
#define NUM_TX_BUFFS 256

static void
sring_rx_check_alignment(void)
{
    struct sring_rx_context *priv = NULL;

    assert(((uintptr_t)&priv->prod) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->cons) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->qmask) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->clear) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->desc[0]) % MY_CACHELINE_SIZE == 0);
}

static void
sring_tx_check_alignment(void)
{
    struct sring_tx_context *priv = NULL;

    assert(NUM_TX_BUFFS % TOTAL_TX_QUEUE_N == 0); /* for now queue count should be multiple of num_tx_buffs */
    assert(((uintptr_t)&priv->prod) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->cons) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->qmask) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&priv->clear) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&sring_tx_context_subqueue_static(NUM_TX_BUFFS/TOTAL_TX_QUEUE_N, 0)->desc[0]) % MY_CACHELINE_SIZE == 0);
}

static size_t
sring_rx_ctx_size(size_t num_rx_bufs)
{
    return sizeof(struct bpfhv_rx_context) + sizeof(struct sring_rx_context) +
	num_rx_bufs * sizeof(struct sring_rx_desc);
}

static size_t
sring_tx_ctx_size(size_t num_tx_bufs)
{
    assert(num_tx_bufs == NUM_TX_BUFFS);
    // TODO: this is horrible and should be generalized
    return sizeof(struct bpfhv_tx_context) + sizeof(struct sring_tx_context) +
        TOTAL_TX_QUEUE_N*(sizeof(struct sring_tx_schqueue_context) + sizeof(struct sring_tx_desc)*(num_tx_bufs/TOTAL_TX_QUEUE_N));
}

static void
sring_rx_ctx_init(struct bpfhv_rx_context *ctx, size_t num_rx_bufs)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;

    assert((num_rx_bufs & (num_rx_bufs - 1)) == 0);
    priv->qmask = num_rx_bufs - 1;
    priv->prod = priv->cons = priv->clear = 0;
    priv->kick_enabled = priv->intr_enabled = 1;
    memset(priv->desc, 0, num_rx_bufs * sizeof(priv->desc[0]));
}

static void
sring_tx_ctx_init(struct bpfhv_tx_context *ctx, size_t num_tx_bufs)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;

    assert((num_tx_bufs & (num_tx_bufs - 1)) == 0);
    priv->qmask = (num_tx_bufs/TOTAL_TX_QUEUE_N) - 1;
    priv->prod = priv->cons = priv->clear = 0;
    priv->kick_enabled = 1;
    priv->intr_at = 0;

    /* init scheduler related context */
    priv->mark = 0;
    priv->add_deficit = 1;
    priv->current_queue = 0;
    priv->total_queued_buffs = 0;
    priv->queue_n = SCHED_TX_QUEUE_N;
    priv->queue_buffs = num_tx_bufs/TOTAL_TX_QUEUE_N;

    struct sring_tx_schqueue_context *txq_main = sring_tx_context_subqueue(priv, 0);
    // this fields are never used
    txq_main->prod = txq_main->cons = txq_main->used = 0;
    txq_main->deficit = 0;
    txq_main->quantum = 0;
    txq_main->weight = 0;
    //
    memset(txq_main->desc, 0, (num_tx_bufs/TOTAL_TX_QUEUE_N)*sizeof(txq_main->desc[0]));

    printf("sring.init queue_n %u queue_buffs %u qmask %u\n",
           ACCESS_ONCE(priv->queue_n), ACCESS_ONCE(priv->queue_buffs),
           ACCESS_ONCE(priv->qmask));

    for(int i=0; i<SCHED_TX_QUEUE_N; i++) {
        struct sring_tx_schqueue_context * scq = sring_tx_context_subqueue(priv, i+1);
        scq->prod = scq->cons = scq->used = 0;
        scq->deficit = 0;
        scq->quantum = 1500;
        scq->weight = i*16+1;
        memset(scq->desc, 0, (num_tx_bufs/TOTAL_TX_QUEUE_N)*sizeof(scq->desc[0]));
    }
}

static inline void
__sring_rxq_notification(struct sring_rx_context *priv, int enable)
{
    priv->kick_enabled = !!enable;
}

static void
sring_rxq_notification(struct bpfhv_rx_context *ctx, int enable)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;

    __sring_rxq_notification(priv, enable);
}

static inline void
__sring_txq_notification(struct sring_tx_context *priv, int enable)
{
    priv->kick_enabled = !!enable;
}

static void
sring_txq_notification(struct bpfhv_tx_context *ctx, int enable)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;

    __sring_txq_notification(priv, enable);
}

static void
sring_rxq_dump(struct bpfhv_rx_context *ctx)
{
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;

    printf("sring.rxq cl %u co %u pr %u kick %u intr %u\n",
           ACCESS_ONCE(priv->clear), ACCESS_ONCE(priv->cons),
           ACCESS_ONCE(priv->prod), ACCESS_ONCE(priv->kick_enabled),
           ACCESS_ONCE(priv->intr_enabled));
}

static void
sring_txq_dump(struct bpfhv_tx_context *ctx)
{
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;

    printf("sring.txq cl %u co %u pr %u kick %u intr_at %u\n",
           ACCESS_ONCE(priv->clear), ACCESS_ONCE(priv->cons),
           ACCESS_ONCE(priv->prod), ACCESS_ONCE(priv->kick_enabled),
           ACCESS_ONCE(priv->intr_at));
}

static size_t
sring_rxq_push(BpfhvBackend *be, BpfhvBackendQueue *rxq,
               int *can_receive)
{
    struct bpfhv_rx_context *ctx = rxq->ctx.rx;
    struct sring_rx_context *priv = (struct sring_rx_context *)ctx->opaque;
    uint32_t prod = ACCESS_ONCE(priv->prod);
    uint32_t cons = priv->cons;
    int count = 0;

    /* Make sure the load of from priv->prod is not delayed after the
     * loads from the ring. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    rxq->notify = 0;

    if (unlikely(priv->kick_enabled)) {
        __sring_rxq_notification(priv, /*enable=*/0);
    }

    for (;;) {
        struct sring_rx_desc *rxd;
        struct iovec iov;
        int pktsize;

        if (unlikely(cons == prod)) {
            /* We ran out of RX descriptors. In busy-wait mode we can just
             * bail out. Otherwise we enable RX kicks and double check for
             * more available descriptors. */
            if (can_receive == NULL) {
                break;
            }
            __sring_rxq_notification(priv, /*enable=*/1);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            prod = ACCESS_ONCE(priv->prod);
            if (cons == prod) {
                /* Not enough space. We need to stop. */
                *can_receive = 0;
                break;
            }
            __sring_rxq_notification(priv, /*enable=*/0);

            /* Make sure the load of from priv->prod is not delayed after the
             * loads from the ring. */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
        }

        if (unlikely(count >= BPFHV_BE_RX_BUDGET)) {
            break;
        }

        rxd = priv->desc + (cons & priv->qmask);
        iov.iov_base = translate_addr(be, rxd->paddr, rxd->len);
        if (unlikely(iov.iov_base == NULL)) {
            /* Invalid descriptor. */
            rxd->len = 0;
            if (verbose) {
                fprintf(stderr, "Invalid RX descriptor: gpa%"PRIx64", "
                                "len %u\n", rxd->paddr, rxd->len);
            }
            cons++;
            continue;
        }
        iov.iov_len = rxd->len;

        /* Read into the scatter-gather buffer referenced by the collected
         * descriptors. */
        pktsize = be->recv(be, &iov, 1);
        if (pktsize <= 0) {
            /* No more data to read (or error). We need to stop. */
            if (unlikely(pktsize < 0 && errno != EAGAIN)) {
                fprintf(stderr, "recv() failed: %s\n", strerror(errno));
            }
            break;
        }

        /* Write back to the receive descriptor effectively used. */
        rxd->len = pktsize;
        rxq->stats.bufs++;
        cons++;
        count++;
    }

    if (count > 0) {
        /* Barrier between store(sring entries) and store(priv->cons). */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        priv->cons = cons;
        /* Full memory barrier to ensure store(priv->cons) happens before
         * load(priv->intr_enabled). See the double-check in sring_rxi().*/
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        rxq->notify = ACCESS_ONCE(priv->intr_enabled);
        rxq->stats.pkts += count;
        rxq->stats.batches++;
    }

    return count;
}

static size_t
sring_txq_drain(BpfhvBackend *be, BpfhvBackendQueue *txq, int *can_send)
{
    struct bpfhv_tx_context *ctx = txq->ctx.tx;
    struct sring_tx_context *priv = (struct sring_tx_context *)ctx->opaque;
    uint32_t prod = ACCESS_ONCE(priv->prod);
    uint32_t cons = priv->cons;
    int count = 0;

    if (can_send) {
        /* Disable further kicks and start processing. */
        __sring_txq_notification(priv, /*enable=*/0);
    }

    /* Make sure the load of from priv->prod is not delayed after the
     * loads from the ring. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    txq->notify = 0;

    for (;;) {
        struct sring_tx_desc *txd = sring_tx_context_subqueue(priv, 0)->desc + (cons & priv->qmask);
        struct iovec iov;
        int ret;

        if (unlikely(cons == prod)) {
            /* Before stopping, check if more work came while we were
             * not looking at priv->prod. */
            prod = ACCESS_ONCE(priv->prod);
            if (cons == prod) {
                /* We ran out of TX descriptors. In busy-wait mode we can just
                 * bail out. Otherwise we enable TX kicks and double check for
                 * more available descriptors. */
                if (can_send == NULL) {
                    break;
                }
                /* Re-enable notifications and double check for
                 * more work. */
                __sring_txq_notification(priv, /*enable=*/1);
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                prod = ACCESS_ONCE(priv->prod);
                if (cons == prod) {
                    break;
                }
                /* More work found: keep going. */
                __sring_txq_notification(priv, /*enable=*/0);
            }

            /* Make sure the load of from priv->prod is not delayed after the
             * loads from the ring. */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
        }

        if (unlikely(count >= BPFHV_BE_TX_BUDGET)) {
            break;
        }

        iov.iov_base = translate_addr(be, txd->paddr, txd->len);
        iov.iov_len = txd->len;
        if (unlikely(iov.iov_base == NULL)) {
            /* Invalid descriptor, just skip it. */
            if (verbose) {
                fprintf(stderr, "Invalid TX descriptor: gpa%"PRIx64", "
                                "len %u\n", txd->paddr, txd->len);
            }
            cons++;
            continue;
        }

        ret = be->send(be, &iov, 1);
        if (unlikely(ret <= 0)) {
            /* Backend is blocked (or failed), so we need to stop.
             * The last packet was not transmitted, so we don't
             * increment 'cons'. */
            if (ret < 0) {
                if (can_send != NULL && errno == EAGAIN) {
                    *can_send = 0;
                } else if (verbose) {
                    fprintf(stderr, "send() failed: %s\n",
                            strerror(errno));
                }
            }
            break;
        }
        txq->stats.bufs++;
        count++;
        cons++;
    }

    if (count > 0) {
        uint32_t old_cons = priv->cons;
        uint32_t intr_at;

        /* Barrier between stores to sring entries and store to priv->cons. */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        priv->cons = cons;
        /* Full memory barrier to ensure store(priv->cons) happens before
         * load(priv->intr_at). See the double-check in sring_txi(). */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        intr_at = ACCESS_ONCE(priv->intr_at);
        txq->notify =
            (uint32_t)(cons - intr_at - 1) < (uint32_t)(cons - old_cons);
        txq->stats.pkts += count;
        txq->stats.batches++;
    }

    return count;
}

BeOps sring_ops = {
    .rx_check_alignment = sring_rx_check_alignment,
    .tx_check_alignment = sring_tx_check_alignment,
    .rx_ctx_size = sring_rx_ctx_size,
    .tx_ctx_size = sring_tx_ctx_size,
    .rx_ctx_init = sring_rx_ctx_init,
    .tx_ctx_init = sring_tx_ctx_init,
    .rxq_push = sring_rxq_push,
    .txq_drain = sring_txq_drain,
    .rxq_kicks = sring_rxq_notification,
    .txq_kicks = sring_txq_notification,
    .rxq_dump = sring_rxq_dump,
    .txq_dump = sring_txq_dump,
    .features_avail = 0,
    .progfile = "proxy/sring_progs.o",
};
