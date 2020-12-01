/*
 * BPFHV paravirtual network device
 *   Definitions shared between the sring eBPF programs and the
 *   sring hv implementation.
 *
 * Copyright (c) 2018 Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __BPFHV_SRING_H__
#define __BPFHV_SRING_H__

#include <stdint.h>

struct sring_tx_desc {
    uint64_t cookie;
    uint64_t paddr;
    uint32_t len;
    uint32_t pad;
};

struct sring_tx_schqueue_context {
    /* Scheduler info */
    uint32_t deficit;
    uint32_t quantum;
    uint32_t weight;

    /* Guest read, write */
    uint32_t prod;
    uint32_t cons;
    uint32_t used;
    uint32_t pad1[26];

    struct sring_tx_desc desc[0];
};

struct sring_tx_context {
    /* Guest write, hv reads. */
    uint32_t prod;
    uint32_t intr_at;
    uint32_t pad1[30];

    /* Guest reads, hv writes. */
    uint32_t cons;
    uint32_t kick_enabled;
    uint32_t pad2[30];

    /* Guest reads, hv reads. */
    uint32_t qmask; // delete this, it should be queue_buffs-1
    uint32_t queue_n;
    uint32_t queue_buffs;
    uint32_t pad3[29];

    /* Private to the guest. */
    uint32_t clear;
    uint32_t mark;
    uint32_t current_queue;
    uint32_t add_deficit;
    uint32_t total_queued_buffs;
    uint32_t pad4[27];

    /* struct sring_tx_schqueue_context queue[0]; */
    /* we cannot use sring_tx_schqueue_context as type because
       it has a dynamic size, defined on malloc. */
    uint8_t queue[0];
};

struct sring_tx_schqueue_context * sring_tx_context_subqueue_static(uint32_t queue_buffs, uint32_t i) {
    struct sring_tx_context* priv = 0;
    return (struct sring_tx_schqueue_context*)
        (&priv->queue + i*(sizeof(struct sring_tx_schqueue_context) + queue_buffs*sizeof(struct sring_tx_desc)));
}

struct sring_tx_schqueue_context * sring_tx_context_subqueue(struct sring_tx_context* priv, uint32_t i) {
    return (struct sring_tx_schqueue_context*)
        (&priv->queue + i*(sizeof(struct sring_tx_schqueue_context) + priv->queue_buffs*sizeof(struct sring_tx_desc)));
}

struct sring_rx_desc {
    uint64_t cookie;
    uint64_t paddr;
    uint32_t len;
    uint32_t pad;
};

struct sring_rx_context {
    /* Guest write, hv reads. */
    uint32_t prod;
    uint32_t intr_enabled;
    uint32_t pad1[30];

    /* Guest reads, hv writes. */
    uint32_t cons;
    uint32_t kick_enabled;
    uint32_t pad2[30];

    /* Guest reads, hv reads. */
    uint32_t qmask;
    uint32_t pad3[31];

    /* Private to the guest. */
    uint32_t clear;
    uint32_t pad4[31];

    struct sring_rx_desc desc[0];
};

#endif  /* __BPFHV_SRING_H__ */
