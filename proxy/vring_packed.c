#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/uio.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>

#include "backend.h"
#include "vring_packed.h"

static void
vring_packed_rx_check_alignment(void)
{
    struct vring_packed_virtq *vq = NULL;

    assert(((uintptr_t)&vq->g.next_free_id) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&vq->state_ofs) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&vq->driver_event) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&vq->device_event) % MY_CACHELINE_SIZE == 0);
    assert(((uintptr_t)&vq->desc[0]) % MY_CACHELINE_SIZE == 0);
    assert(sizeof(vq->driver_event) == 4);
    assert(sizeof(vq->device_event) == 4);
}

static void
vring_packed_tx_check_alignment(void)
{
    vring_packed_rx_check_alignment();
}

static size_t
vring_packed_priv_size(size_t num_bufs)
{
    size_t desc_size = ROUNDUP(sizeof(struct vring_packed_desc) * num_bufs,
                               MY_CACHELINE_SIZE);
    size_t state_size = ROUNDUP(
                    sizeof(struct vring_packed_desc_state) * num_bufs,
                    MY_CACHELINE_SIZE);
    size_t hv_map_size = ROUNDUP(
                    sizeof(struct vring_packed_desc_hv_map) * num_bufs,
                    MY_CACHELINE_SIZE);

    return sizeof(struct vring_packed_virtq) + desc_size
                                        + state_size + hv_map_size;
}

static size_t
vring_packed_rx_ctx_size(size_t num_rx_bufs)
{
    return sizeof(struct bpfhv_rx_context)
            + vring_packed_priv_size(num_rx_bufs);
}

static size_t
vring_packed_tx_ctx_size(size_t num_tx_bufs)
{
    return sizeof(struct bpfhv_tx_context)
            + vring_packed_priv_size(num_tx_bufs);
}

static inline void
vring_packed_notification(struct vring_packed_virtq *vq, int enable)
{
    if (!enable) {
        ACCESS_ONCE(vq->device_event.flags) = vq->h.device_event_flags =
                            VRING_PACKED_EVENT_FLAG_DISABLE;
    } else {
#if 0
        /* Enable this to let the host suppress guest --> host notifications
         * using the simpler interrupt bit scheme, rather than the event idx
         * feature. */
        ACCESS_ONCE(vq->device_event.flags) = vq->h.device_event_flags =
                            VRING_PACKED_EVENT_FLAG_ENABLE;
#else
        union vring_packed_desc_event device_event;

        device_event.off_wrap = vq->h.next_avail_idx |
            (vq->h.avail_wrap_counter << VRING_PACKED_EVENT_F_WRAP_CTR);
        device_event.flags = VRING_PACKED_EVENT_FLAG_DESC;

        /* Use a single (atomic) store to write to vq->device_event. Using two
         * stores would require a release barrier, because we need to guarantee
         * that the update to the flags field is not visible before the update
         * to the off_wrap field. */
        ACCESS_ONCE(vq->device_event.u32) = device_event.u32;
        vq->h.device_event_flags = device_event.flags;
#endif
    }
}

static void
vring_packed_init(struct vring_packed_virtq *vq, size_t num)
{
    size_t desc_size = ROUNDUP(sizeof(struct vring_packed_desc) * num,
                               MY_CACHELINE_SIZE);
    size_t state_desc_size = ROUNDUP(sizeof(struct vring_packed_desc_state) * num,
                               MY_CACHELINE_SIZE);
    struct vring_packed_desc_state *state;
    unsigned int i;

    memset(vq, 0, vring_packed_priv_size(num));

    vq->g.next_free_id = 0;
    vq->g.next_avail_idx = 0;
    vq->g.next_used_idx = 0;
    vq->g.avail_wrap_counter = 1;
    vq->g.used_wrap_counter = 1;
    vq->g.avail_used_flags = 1 << VRING_PACKED_DESC_F_AVAIL;
    vq->g.pending_inuse_counter = 0;
    vq->g.pending_used_counter = 0;
    vq->g.avail_dropped = 0;

    vq->h.next_avail_idx = 0;
    vq->h.next_used_idx = 0;
    vq->h.avail_wrap_counter = 1;
    vq->h.used_wrap_counter = 1;
    vq->h.avail_used_flags = 1 << VRING_PACKED_DESC_F_AVAIL |
                             1 << VRING_PACKED_DESC_F_USED;


    vq->num_desc = num;
    vq->state_ofs = sizeof(struct vring_packed_virtq) + desc_size;
    vq->hv_map_ofs = vq->state_ofs + state_desc_size;

    vq->driver_event.flags = VRING_PACKED_EVENT_FLAG_DESC;
    vq->driver_event.off_wrap = 1 << VRING_PACKED_EVENT_F_WRAP_CTR;
    vring_packed_notification(vq, /*enable=*/1);

    state = vring_packed_state(vq);
    for (i = 0; i < num-1; i++) {
        state[i].next = i + 1;
        state[i].busy = 0;
    }
}

static void
vring_packed_rx_ctx_init(struct bpfhv_rx_context *ctx, size_t num_rx_bufs)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    vring_packed_init(vq, num_rx_bufs);
}

static void
vring_packed_tx_ctx_init(struct bpfhv_tx_context *ctx, size_t num_tx_bufs)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    vring_packed_init(vq, num_tx_bufs);
}

static void
vring_packed_tx_ctx_init_mark(struct bpfhv_tx_context *ctx, uint mark_mode)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    vq->mark_on_guest = (mark_mode == MARK_MODE_GUEST);
}

static void
vring_packed_rxq_notification(struct bpfhv_rx_context *ctx, int enable)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    vring_packed_notification(vq, enable);
}

static void
vring_packed_txq_notification(struct bpfhv_tx_context *ctx, int enable)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    vring_packed_notification(vq, enable);
}

static void
vring_packed_dump(struct vring_packed_virtq *vq, const char *suffix)
{
    printf("vringpacked.%s g.avl %u g.usd %u g.wrp %u:%u dri %u:%u:%u "
            "h.avl %u h.usd %u h.wrp %u:%u dev %u:%u:%u\n",
            suffix, vq->g.next_avail_idx, vq->g.next_used_idx,
            vq->g.avail_wrap_counter, vq->g.used_wrap_counter,
            vq->driver_event.flags,
            vq->driver_event.off_wrap >> VRING_PACKED_EVENT_F_WRAP_CTR,
            vq->driver_event.off_wrap & ~(1 << VRING_PACKED_EVENT_F_WRAP_CTR),
            vq->h.next_avail_idx, vq->h.next_used_idx,
            vq->h.avail_wrap_counter, vq->h.used_wrap_counter,
            vq->device_event.flags,
            vq->device_event.off_wrap >> VRING_PACKED_EVENT_F_WRAP_CTR,
            vq->device_event.off_wrap & ~(1 << VRING_PACKED_EVENT_F_WRAP_CTR));
}

static void
vring_packed_rxq_dump(struct bpfhv_rx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    vring_packed_dump(vq, "rxq");
}

static void
vring_packed_txq_dump(struct bpfhv_tx_context *ctx)
{
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;

    vring_packed_dump(vq, "txq");
}

static inline int
vring_packed_more_avail(struct vring_packed_virtq *vq)
{
    uint16_t flags = vq->desc[vq->h.next_avail_idx].flags;
    int avail, used;

    avail = !!(flags & (1 << VRING_PACKED_DESC_F_AVAIL));
    used = !!(flags & (1 << VRING_PACKED_DESC_F_USED));

    return avail != used && avail == vq->h.avail_wrap_counter;
}

static inline void
vring_packed_advance_avail(struct vring_packed_virtq *vq)
{
    if (unlikely(++vq->h.next_avail_idx >= vq->num_desc)) {
        vq->h.next_avail_idx = 0;
        vq->h.avail_wrap_counter ^= 1;
    }
}

static inline void
vring_packed_advance_used(struct vring_packed_virtq *vq)
{
    if (unlikely(++vq->h.next_used_idx >= vq->num_desc)) {
        vq->h.next_used_idx = 0;
        vq->h.used_wrap_counter ^= 1;
        vq->h.avail_used_flags ^= 1 << VRING_PACKED_DESC_F_AVAIL |
                                  1 << VRING_PACKED_DESC_F_USED;
    }
}

static inline int
vring_packed_intr_needed(struct vring_packed_virtq *vq, uint16_t num_consumed)
{
    uint16_t old_idx, event_idx, wrap_counter;
    union vring_packed_desc_event driver_event;

    /* Read off_wrap and flags with a single (atomic) load operation, to avoid
     * a race condition that would require an acquire barrier. */
    driver_event.u32 = vq->driver_event.u32;

    if (driver_event.flags != VRING_PACKED_EVENT_FLAG_DESC) {
        return driver_event.flags == VRING_PACKED_EVENT_FLAG_ENABLE;
    }

    /* Rebase the old used idx and the event idx in the frame of the current
     * used idx, so that we can use the usual vring_need_event() macro. */
    old_idx = vq->h.next_used_idx - num_consumed;
    event_idx = driver_event.off_wrap & ~(1 << VRING_PACKED_EVENT_F_WRAP_CTR);
    wrap_counter = driver_event.off_wrap >> VRING_PACKED_EVENT_F_WRAP_CTR;
    if (wrap_counter != vq->h.used_wrap_counter) {
        event_idx -= vq->num_desc;
    }

    return vring_need_event(old_idx, event_idx, vq->h.next_used_idx);
}

static size_t
vring_packed_rxq_push(BpfhvBackend *be, BpfhvBackendQueue *rxq,
                      int *can_receive)
{
    struct bpfhv_rx_context *ctx = rxq->ctx.rx;
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    size_t count = 0;

    if (unlikely(vq->h.device_event_flags != VRING_PACKED_EVENT_FLAG_DISABLE)) {
        vring_packed_notification(vq, /*enable=*/0);
    }

    rxq->notify = 0;

    for (;;) {
        uint16_t avail_idx = vq->h.next_avail_idx;
        uint16_t used_idx = vq->h.next_used_idx;
        struct vring_packed_desc *desc;
        struct iovec iov;
        ssize_t pktsize;

        if (!vring_packed_more_avail(vq)) {
            /* We ran out of RX descriptors. In busy-wait mode we can just
             * bail out. Otherwise we enable RX kicks and double check for
             * more available descriptors. */
            if (can_receive == NULL) {
                break;
            }
            vring_packed_notification(vq, /*enable=*/1);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (!vring_packed_more_avail(vq)) {
                break;
            }
            vring_packed_notification(vq, /*enable=*/0);
        }

        if (unlikely(count >= BPFHV_BE_RX_BUDGET)) {
            break;
        }

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        desc = vq->desc + avail_idx;
        iov.iov_base = translate_addr(be, desc->addr, desc->len);
        if (unlikely(avail_idx != used_idx)) {
            /* Descriptor rewrite is needed only in case of out of order,
             * processing (not implemented). */
            vq->desc[used_idx] = *desc;
        }
        if (unlikely(iov.iov_base == NULL)) {
            /* Invalid descriptor. */
            vq->desc[used_idx].len = 0;
            if (verbose) {
                fprintf(stderr, "Invalid RX descriptor: gpa%"PRIx64", "
                                "len %u\n", desc->addr, desc->len);
            }
        } else {
            iov.iov_len = desc->len;
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

            /* Write back to the receive descriptor used. */
            vq->desc[used_idx].len = pktsize;
        }

        /* Expose the used descriptor, and advance avail and used indices. */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        vq->desc[used_idx].flags = vq->h.avail_used_flags;
        vring_packed_advance_avail(vq);
        vring_packed_advance_used(vq);
        rxq->stats.bufs++;
        count++;
    }

    if (count > 0) {
        /* Store-load barrier to make sure stores to the descriptors
         * happen before the load from vq->driver_event */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        rxq->notify = vring_packed_intr_needed(vq, count);
        rxq->stats.pkts += count;
        rxq->stats.batches++;
    }

    return count;
}

static size_t
vring_packed_txq_drain(BpfhvBackend *be, BpfhvBackendQueue *txq, int *can_send)
{
    struct bpfhv_tx_context *ctx = txq->ctx.tx;
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    size_t count = 0;

    if (can_send) {
        /* Disable further kicks and start processing. */
        vring_packed_notification(vq, /*enable=*/0);
    }

    txq->notify = 0;

    for (;;) {
        uint16_t avail_idx = vq->h.next_avail_idx;
        uint16_t used_idx = vq->h.next_used_idx;
        struct iovec iov;

        if (!vring_packed_more_avail(vq)) {
            /* We ran out of TX descriptors. In busy-wait mode we can just
             * bail out. Otherwise we enable TX kicks and double check for
             * more available descriptors. */
            if (can_send == NULL) {
                break;
            }
            vring_packed_notification(vq, /*enable=*/1);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (!vring_packed_more_avail(vq)) {
                break;
            }
            vring_packed_notification(vq, /*enable=*/0);
        }

        if (unlikely(count >= BPFHV_BE_TX_BUDGET)) {
            break;
        }

        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        /* Get the next avail descriptor and process it. */
        iov.iov_base = translate_addr(be, vq->desc[avail_idx].addr,
                                      vq->desc[avail_idx].len);
        iov.iov_len = vq->desc[avail_idx].len;
        if (unlikely(iov.iov_base == NULL)) {
            /* Invalid descriptor, just skip it. */
            if (verbose) {
                fprintf(stderr, "Invalid TX descriptor: gpa%"PRIx64", "
                                "len %u\n", vq->desc[avail_idx].addr,
                                vq->desc[avail_idx].len);
            }
        } else {
            int ret = be->send(be, &iov, 1);

            if (unlikely(ret <= 0)) {
                /* Backend is blocked (or failed), so we need to stop.
                 * The last packet was not transmitted, so we don't
                 * increment 'avail_idx'. */
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
        }

        /* Fill the next used descriptor and expose it. Don't rewrite the
         * descriptor addr and len if not necessary (avail_idx != used_idx
         * means that we processed descriptor out of order, which is not
         * the case here). This is specially useful to avoid a store-store
         * memory barrier. */
        if (unlikely(avail_idx != used_idx)) {
            vq->desc[used_idx] = vq->desc[avail_idx];
            __atomic_thread_fence(__ATOMIC_RELEASE);
        }
        vq->desc[used_idx].flags = vq->h.avail_used_flags;

        /* Advance avail and used indices. */
        vring_packed_advance_avail(vq);
        vring_packed_advance_used(vq);

        txq->stats.bufs++;
        count++;
    }

    if (count > 0) {
        /* Flush previous writes to the descriptor flags, and
         * then check if the peer needs to be notified. */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        txq->notify = vring_packed_intr_needed(vq, count);
        txq->stats.pkts += count;
        txq->stats.batches++;
    }

    return count;
}

/* return number of acquired packets */
static size_t
vring_packed_txq_acquire(BpfhvBackend *be, BpfhvBackendQueue *txq, int *can_send, size_t *dropped)
{
    struct bpfhv_tx_context *ctx = txq->ctx.tx;
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    size_t count = 0, _dropped = 0;
    struct BpfhvBackendProcess *bp = be->parent_bp;
    uint32_t mark;

    if (can_send) {
        /* Disable further kicks and start processing. */
        vring_packed_notification(vq, /*enable=*/0);
    }

    txq->notify = 0;

    for (;;) {
        uint16_t avail_idx = vq->h.next_avail_idx;
        struct iovec iov;

        if (!vring_packed_more_avail(vq)) {
            /* We ran out of TX descriptors. In busy-wait mode we can just
             * bail out. Otherwise we enable TX kicks and double check for
             * more available descriptors. */
            if (can_send == NULL) {
                *dropped = _dropped;
                break;
            }
            vring_packed_notification(vq, /*enable=*/1);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (!vring_packed_more_avail(vq)) {
                *dropped = _dropped;
                break;
            }
            vring_packed_notification(vq, /*enable=*/0);
        }

        if (unlikely(count >= BPFHV_BE_TX_BUDGET)) {
            break;
        }

        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        /* Get the next avail descriptor and process it. */
        struct vring_packed_desc *avail_desc = &vq->desc[avail_idx];
        iov.iov_base = translate_addr(be, avail_desc->addr, avail_desc->len);
        iov.iov_len = avail_desc->len;
        if (unlikely(iov.iov_base == NULL)) {
            /* Invalid descriptor, just skip it. */
            if (verbose) {
                fprintf(stderr, "Invalid TX descriptor: gpa%"PRIx64", "
                                "len %u\n", avail_desc->addr,
                                avail_desc->len);
            }
            /* TODO: should we release/notify here? */
        } else {
            /* Save buffer id ring slot into hv_map. We can't pass slot ids to
             * release buffers because slots can be swapped, so we have to map
             * buffer ids to slot ids */
            struct vring_packed_desc_hv_map *hvmap = vring_packed_hv_map(vq);
            hvmap[avail_desc->id].slot_idx = avail_idx;

            /* Implement mark mode. Mark here if MARK_MODE_HV is selected,
             * use guest provided mark if MARK_MODE_GUEST is used, use
             * null mark if no mark is selected. */
            switch(bp->mark_mode) {
                case MARK_MODE_GUEST:
                    mark = avail_desc->mark;
                    break;
                case MARK_MODE_HV:
                    mark = bp->hv_mark_pkt_fun(iov.iov_base, iov.iov_len);
                    break;
                case MARK_MODE_NO_MARK:
                    mark = 0;
                    break;
                default: assert(0);
            }

            /* Although iov can be easily obtained from descriptor at avail_idx,
             * we don't do that to avoid further cacheline bouncing made by the
             * scheduler thread. iov is passed by value. */
            int ret = bp->sched_enqueue(bp, be, txq, iov, avail_desc->id, mark);

            /* release dropped packet (caller cannot not do it for us) */
            if (unlikely(ret > 0)) {
                if (unlikely(verbose))
                    fprintf(stderr, "Dropped packet on sched_enqueue!\n");
                vring_packed_ops.txq_release(be, txq, avail_desc->id);
                /* delay/group dropped packet notifications */
                _dropped++;
                /* update stats */
                vq->g.avail_dropped++;
            }

            /* TODO: can_send does not have any meaning when enqueuing to scheduler */
            // if (unlikely(ret <= 0)) {
            //     /* Backend is blocked (or failed), so we need to stop.
            //      * The last packet was not transmitted, so we don't
            //      * increment 'avail_idx'. */
            //     if (ret < 0) {
            //         if (can_send != NULL && errno == EAGAIN) {
            //             *can_send = 0;
            //         } else if (verbose) {
            //             fprintf(stderr, "send() failed: %s\n",
            //                     strerror(errno));
            //         }
            //     }
            //     break;
            // }
        }

        /* Count pending buffers that need to be set as used at some point */
        vq->g.pending_inuse_counter++;

        /* Advance avail index */
        vring_packed_advance_avail(vq);

        txq->stats.bufs++;
        count++;
    }

    return count;
}

/* optimized swap: only addr, len and id are swapped. flags remain the same (maybe complemented)
 * for acquired unreleased descriptors, and mark can be skipped because it's useless after enqueuing */
static inline void
vring_packed_desc_swap_acquired(struct vring_packed_virtq *vq, uint16_t idx1, uint16_t idx2) {
    struct vring_packed_desc temp;
    struct vring_packed_desc *desc1 = &vq->desc[idx1];
    struct vring_packed_desc *desc2 = &vq->desc[idx2];
    temp.addr = desc1->addr;
    temp.len = desc1->len;
    temp.id = desc1->id;

    desc1->addr = desc2->addr;
    desc1->len = desc2->len;
    desc1->id = desc2->id;

    desc2->addr = temp.addr;
    desc2->len = temp.len;
    desc2->id = temp.id;

    /* note on flags: flags field of slots at idx1 and idx2 are not generally equal:
     * if used_idx wrapped and id2 < id1 then flags are complemented. However
     * acquired but not released descriptors must be always not available and not
     * used, hence swapping descriptors without copying flags will yield
     * the same status (available, not used) */
    uint64_t used_idx = idx1;
    uint64_t _idx = idx2;
    uint64_t mergeflags = 1 << VRING_PACKED_DESC_F_AVAIL | 1 << VRING_PACKED_DESC_F_USED;
    assert( (used_idx == _idx) ||
            (used_idx < _idx && desc1->flags == desc2->flags) ||
            (used_idx > _idx && (desc1->flags ^ desc2->flags) == mergeflags));
}

static void
vring_packed_txq_release(BpfhvBackend *be, BpfhvBackendQueue *txq, uint64_t opaque_id)
{
    struct bpfhv_tx_context *ctx = txq->ctx.tx;
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    uint16_t used_idx = vq->h.next_used_idx;
    uint16_t id = (uint16_t)opaque_id, _idx;

    /* we can only release an unreleased buf (bug) */
    if(unlikely(vq->g.pending_inuse_counter == 0))
        return;

    /* hv_map : bufferid -> slot_index */
    struct vring_packed_desc_hv_map *hv_map = vring_packed_hv_map(vq);
    _idx = hv_map[id].slot_idx;

    /* avoid ring write and write-write mfence if not needed */
    if (_idx != used_idx) {
        /* In this case descriptors at used_idx and _idx must be swapped.
         * Also, slot index associated to used_idx buffer must be updated */
        struct vring_packed_desc *used_desc = &vq->desc[used_idx];
        hv_map[used_desc->id].slot_idx = _idx;

        /* swap by copying descriptors content */
        vring_packed_desc_swap_acquired(vq, used_idx, _idx);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        //fprintf(stderr, "ooo %lu\n", opaque_id);
    }
    vq->desc[used_idx].flags = vq->h.avail_used_flags;

    /* update pending counter and advance hv used index */
    vq->g.pending_inuse_counter--;
    vring_packed_advance_used(vq);

    /* delay notification, keep how many buffers to notify */
    vq->g.pending_used_counter++;
}

static size_t
vring_packed_txq_notify(BpfhvBackend *be, BpfhvBackendQueue *txq)
{
    struct bpfhv_tx_context *ctx = txq->ctx.tx;
    struct vring_packed_virtq *vq = (struct vring_packed_virtq *)ctx->opaque;
    uint32_t count = vq->g.pending_used_counter;
    if(count > 0)
    {
        /* Flush previous writes to the descriptor flags, and
         * then check if the peer needs to be notified. */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        txq->notify = vring_packed_intr_needed(vq, count);
        txq->stats.pkts += count;
        txq->stats.batches++;

        vq->g.pending_used_counter = 0;
    }
    return count;
}

BeOps vring_packed_ops = {
    .rx_check_alignment = vring_packed_rx_check_alignment,
    .tx_check_alignment = vring_packed_tx_check_alignment,
    .rx_ctx_size = vring_packed_rx_ctx_size,
    .tx_ctx_size = vring_packed_tx_ctx_size,
    .rx_ctx_init = vring_packed_rx_ctx_init,
    .tx_ctx_init = vring_packed_tx_ctx_init,
    .tx_ctx_init_mark = vring_packed_tx_ctx_init_mark,
    .rxq_kicks = vring_packed_rxq_notification,
    .txq_kicks = vring_packed_txq_notification,
    .rxq_push = vring_packed_rxq_push,
    .txq_drain = vring_packed_txq_drain,
    .txq_acquire = vring_packed_txq_acquire,
    .txq_release = vring_packed_txq_release,
    .txq_notify = vring_packed_txq_notify,
    .rxq_dump = vring_packed_rxq_dump,
    .txq_dump = vring_packed_txq_dump,
    .features_avail = 0,
    .progfile = "proxy/vring_packed_progs.o",
};