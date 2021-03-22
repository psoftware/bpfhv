#include "bpfhv.h"
#include "vring_packed.h"

#ifndef __section
# define __section(NAME)                  \
   __attribute__((section(NAME), used))
#endif

static int BPFHV_FUNC(rx_pkt_alloc, struct bpfhv_rx_context *ctx);
static int BPFHV_FUNC(smp_mb_full);

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))
#define compiler_barrier() __asm__ __volatile__ ("");
#define smp_mb_release()    compiler_barrier()
#define smp_mb_acquire()    compiler_barrier()

static inline void
vring_packed_add(struct vring_packed_virtq *vq, struct bpfhv_buf *b,
                 uint16_t flags, uint32_t mark)
{
    struct vring_packed_desc_state *state = vring_packed_state(vq);
    uint16_t head_avail_idx;
    uint16_t head_flags;
    uint16_t avail_idx;
    uint16_t state_idx;
    uint16_t id;

    head_avail_idx = avail_idx = vq->g.next_avail_idx;
    head_flags = vq->g.avail_used_flags | flags;
    id = state_idx = vq->g.next_free_id;

    vq->desc[avail_idx].addr = b->paddr;
    vq->desc[avail_idx].len = b->len;
    vq->desc[avail_idx].id = id;
    vq->desc[avail_idx].mark = mark;
    state[state_idx].cookie = b->cookie;

    if (++avail_idx >= vq->num_desc) {
        avail_idx = 0;
        vq->g.avail_used_flags ^= 1 << VRING_PACKED_DESC_F_AVAIL |
                                1 << VRING_PACKED_DESC_F_USED;
        vq->g.avail_wrap_counter ^= 1;
    }

    vq->g.next_avail_idx = avail_idx;
    vq->g.next_free_id = state[id].next;
    state[id].busy = 1;
#if 0
    state[id].num = 1;
    state[id].last = id;
#endif

    /* Publish the new descriptor chain by exposing the flags of the first
     * descriptor in the chain. */
    smp_mb_release();
    vq->desc[head_avail_idx].flags = head_flags;
}

/* Check if the hypervisor needs a notification. */
static inline int
vring_packed_kick_needed(struct vring_packed_virtq *vq, uint16_t num_published)
{
    uint16_t old_idx, event_idx, wrap_counter;
    union vring_packed_desc_event device_event;

    /* Read off_wrap and flags with a single (atomic) load operation, to avoid
     * a race condition that would require an acquire barrier. */
    device_event.u32 = ACCESS_ONCE(vq->device_event.u32);

    if (device_event.flags != VRING_PACKED_EVENT_FLAG_DESC) {
        return device_event.flags == VRING_PACKED_EVENT_FLAG_ENABLE;
    }

    /* Rebase the old avail idx and the event idx in the frame of the current
     * avail idx, so that we can use the usual vring_need_event() macro. */
    old_idx = vq->g.next_avail_idx - num_published;
    event_idx = device_event.off_wrap & ~(1 << VRING_PACKED_EVENT_F_WRAP_CTR);
    wrap_counter = device_event.off_wrap >> VRING_PACKED_EVENT_F_WRAP_CTR;
    if (wrap_counter != vq->g.avail_wrap_counter) {
        event_idx -= vq->num_desc;
    }

    return vring_need_event(old_idx, event_idx, vq->g.next_avail_idx);
}

static inline uint32_t
mark_packet(struct bpfhv_tx_context *ctx) {
    return 0;
}

__section("txp")
int vring_packed_txp(struct bpfhv_tx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    struct bpfhv_buf *txb = ctx->bufs + 0;

    if (ctx->num_bufs != 1) {
        return -1;
    }

    uint32_t mark = mark_packet(ctx);
    vring_packed_add(vq, txb, 0, mark);
    smp_mb_full();
    ctx->oflags = vring_packed_kick_needed(vq, 1) ? BPFHV_OFLAGS_KICK_NEEDED : 0;

    return 0;
}

static inline int
vring_packed_desc_is_used(struct vring_packed_virtq *vq, uint16_t used_idx,
                          uint8_t used_wrap_counter)
{
    uint16_t flags = vq->desc[used_idx].flags;
    int avail, used;

    avail = !!(flags & (1 << VRING_PACKED_DESC_F_AVAIL));
    used = !!(flags & (1 << VRING_PACKED_DESC_F_USED));

    return avail == used && used == used_wrap_counter;
}

static inline int
vring_packed_more_used(struct vring_packed_virtq *vq)
{
    return vring_packed_desc_is_used(vq, vq->g.next_used_idx,
                                     vq->g.used_wrap_counter);
}

static inline int
vring_packed_more_pending(struct vring_packed_virtq *vq)
{
    uint16_t flags = vq->desc[vq->g.next_used_idx].flags;
    int avail, used;

    avail = !!(flags & (1 << VRING_PACKED_DESC_F_AVAIL));
    used = !!(flags & (1 << VRING_PACKED_DESC_F_USED));

    smp_mb_acquire();

    return avail != used && used != vq->g.used_wrap_counter;
}

/* Consume the next used entry. It is up to the caller to check that
 * we can actually do that. This returns -1 in case of error, and
 * 1 in case of success (since it is more convenient for the caller). */
static inline int
vring_packed_get(struct vring_packed_virtq *vq, struct bpfhv_buf *b)
{
    struct vring_packed_desc_state *state = vring_packed_state(vq);
    uint16_t used_idx;
    uint16_t id;

    used_idx = vq->g.next_used_idx;
    id = vq->desc[used_idx].id;
    if (id >= vq->num_desc || !state[id].busy) {
        return -1;  /* This is a bug. */
    }

    b->cookie = state[id].cookie;
    b->paddr = vq->desc[used_idx].addr;
    b->len = vq->desc[used_idx].len;

    state[id].busy = 0;
    state[id].next = vq->g.next_free_id;
    vq->g.next_free_id = id;

    if (++used_idx >= vq->num_desc) {
        used_idx = 0;
        vq->g.used_wrap_counter ^= 1;
    }
    vq->g.next_used_idx = used_idx;

    return 1;
}

static inline int
vring_packed_intr(struct vring_packed_virtq *vq, uint16_t min_completed_bufs)
{
    uint16_t used_wrap_counter = vq->g.used_wrap_counter;
    union vring_packed_desc_event driver_event;
    uint16_t used_idx = vq->g.next_used_idx;

    if (min_completed_bufs == 0) {
        vq->driver_event.flags = VRING_PACKED_EVENT_FLAG_DISABLE;
        return 0;
    }

    if (min_completed_bufs > vq->num_desc) {
        return -1;
    }

    used_idx += min_completed_bufs - 1;
    if (used_idx >= vq->num_desc) {
        used_idx -= vq->num_desc;
        used_wrap_counter ^= 1;
    }

    driver_event.off_wrap = used_idx |
        (used_wrap_counter << VRING_PACKED_EVENT_F_WRAP_CTR);
    driver_event.flags = VRING_PACKED_EVENT_FLAG_DESC;

    /* Use a single (atomic) store to write to vq->driver_event. Using two
     * stores would require a release barrier, because we need to guarantee
     * that the update to the flags field is not visible before the update
     * to the off_wrap field. */
    vq->driver_event.u32 = driver_event.u32;

    smp_mb_full();

    return vring_packed_desc_is_used(vq, used_idx, used_wrap_counter);
}

__section("txc")
int vring_packed_txc(struct bpfhv_tx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    struct bpfhv_buf *txb = ctx->bufs + 0;
    int ret;

    if (!vring_packed_more_used(vq)) {
        return 0;
    }
    smp_mb_acquire();

    ret = vring_packed_get(vq, txb);
    if (ret == 1) {
        ctx->num_bufs = 1;
        ctx->oflags = 0;
    }

    return ret;
}

__section("txr")
int vring_packed_txr(struct bpfhv_tx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    struct bpfhv_buf *txb = ctx->bufs + 0;
    int ret;

    if (!vring_packed_more_pending(vq)) {
        return 0;
    }

    ret = vring_packed_get(vq, txb);
    if (ret == 1) {
        ctx->num_bufs = 1;
        ctx->oflags = 0;
    }

    return ret;
}

__section("txi")
int vring_packed_txi(struct bpfhv_tx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    return vring_packed_intr(vq, ctx->min_completed_bufs);
}

__section("rxp")
int vring_packed_rxp(struct bpfhv_rx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    unsigned int i;

    if (ctx->num_bufs > BPFHV_MAX_RX_BUFS) {
        return -1;
    }

    for (i = 0; i < ctx->num_bufs; i++) {
        struct bpfhv_buf *rxb = ctx->bufs + i;

        vring_packed_add(vq, rxb, VRING_DESC_F_WRITE, 0);

    }
    smp_mb_full();
    ctx->oflags = vring_packed_kick_needed(vq, i) ? BPFHV_OFLAGS_KICK_NEEDED : 0;

    return 0;
}

__section("rxc")
int vring_packed_rxc(struct bpfhv_rx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    struct bpfhv_buf *rxb = ctx->bufs + 0;
    int ret;

    if (!vring_packed_more_used(vq)) {
        return 0;
    }
    smp_mb_acquire();

    ret = vring_packed_get(vq, rxb);
    if (ret != 1) {
        return ret;
    }

    ctx->num_bufs = 1;
    ctx->oflags = 0;

    ret = rx_pkt_alloc(ctx);
    if (ret < 0) {
        return ret;
    }

    return 1;
}

__section("rxr")
int vring_packed_rxr(struct bpfhv_rx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    unsigned int i;

    if (!vring_packed_more_pending(vq)) {
        return 0;
    }

    for (i = 0; i < BPFHV_MAX_RX_BUFS && vring_packed_more_pending(vq); i++) {
        struct bpfhv_buf *rxb = ctx->bufs + i;
        int ret;

        ret = vring_packed_get(vq, rxb);
        if (ret < 0) {
            if (i == 0) {
                return ret;
            }
            break;
        }
    }

    ctx->num_bufs = i;
    ctx->oflags = 0;

    return 1;
}

__section("rxi")
int vring_packed_rxi(struct bpfhv_rx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    return vring_packed_intr(vq, ctx->min_completed_bufs);
}
