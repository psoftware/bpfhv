#ifndef __BPFHV_PROXY_H__
#define __BPFHV_PROXY_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BPFHV_PROXY_MAX_REGIONS     8
#define BPFHV_PROXY_DIRECTION_RX    1
#define BPFHV_PROXY_DIRECTION_TX    2

typedef struct BpfhvProxyMemoryRegion {
    uint64_t        guest_physical_addr;
    uint64_t        size;
    uint64_t        hypervisor_virtual_addr;
    uint64_t        mmap_offset;
} BpfhvProxyMemoryRegion;

typedef struct BpfhvProxyMemoryMap {
    uint32_t                num_regions;
    uint32_t                pad;
    BpfhvProxyMemoryRegion  regions[BPFHV_PROXY_MAX_REGIONS];
} BpfhvProxyMemoryMap;

typedef struct BpfhvProxyQueueCtx {
    uint32_t            queue_idx;
    uint32_t            direction;
    uint64_t            guest_physical_addr;
} BpfhvProxyQueueCtx;

typedef struct BpfhvProxyNotifier {
    uint32_t            queue_idx;
    uint32_t            direction;
} BpfhvProxyNotifier;

typedef enum BpfhvProxyReqType {
    BPFHV_PROXY_REQ_NONE = 0,
    BPFHV_PROXY_REQ_GET_FEATURES,
    BPFHV_PROXY_REQ_SET_FEATURES,
    BPFHV_PROXY_REQ_GET_PROGRAMS,
    BPFHV_PROXY_REQ_SET_MEM_TABLE,
    BPFHV_PROXY_REQ_SET_QUEUE_CTX,
    BPFHV_PROXY_REQ_SET_QUEUE_KICK,
    BPFHV_PROXY_REQ_SET_QUEUE_IRQ,
    BPFHV_PROXY_REQ_RX_ENABLE,
    BPFHV_PROXY_REQ_TX_ENABLE,
    BPFHV_PROXY_REQ_RX_DISABLE,
    BPFHV_PROXY_REQ_TX_DISABLE,
} BpfhvProxyReqType;

typedef union BpfhvProxyMsgPayload {
    uint64_t            u64;
    /* Associated messages:
     *   - BPFHV_PROXY_REQ_GET_FEATURES (resp)
     *   - BPFHV_PROXY_REQ_SET_FEATURES
     */

    BpfhvProxyMemoryMap memory_map;
    /* Associated messages:
     *   - BPFHV_PROXY_REQ_SET_MEM_TABLE
     */

    BpfhvProxyQueueCtx  queue_ctx;
    /* Associated messages:
     *   - BPFHV_PROXY_REQ_SET_QUEUE_CTX
     */

    BpfhvProxyNotifier  notify;
    /* Associated messages:
     *   - BPFHV_PROXY_REQ_SET_QUEUE_KICK
     *   - BPFHV_PROXY_REQ_SET_QUEUE_IRQ
     */
} BpfhvProxyMsgPayload;

typedef struct BpfhvProxyMsgHeader {
    BpfhvProxyReqType       reqtype;
    uint32_t                flags;
    uint32_t                size;
} BpfhvProxyMsgHeader;

typedef struct BpfhvProxyMessage {
    BpfhvProxyMsgHeader     hdr;
    BpfhvProxyMsgPayload    payload;
} __attribute__((packed)) BpfhvProxyMessage;

#ifdef __cplusplus
}
#endif

#endif  /* __BPFHV_PROXY_H__ */