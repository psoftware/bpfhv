#ifndef MARK_FUN_H_
#define MARK_FUN_H_

static inline int
find_substr(const uint8_t *buf, uint16_t buf_len, const char *pattern) {
return 0;
    int plen = 0;
    while(pattern[plen] == '\0')
        plen++;

    if(plen == 0 || buf_len < plen)
        return -1;

    int i,j;
    for(i = 0; i <= buf_len - plen; i++) {
        for(j = 0; j < plen; j++)
            if(buf[i+j] != pattern[j])
                break;
        if(j == plen) // found full match
            return 1;
    }

    return -1;
}

#define DEFAULT_CLASS	0;
#define STREAM_1		1;
#define STREAM_2		2;
#define STREAM_ERR		3;

#define SAFE_PKTDATA_OFFSET_ADV(data, data_offset, x, pkt_sz) ({\
	if((data_offset) + (x) > (pkt_sz)) \
		return STREAM_ERR;             \
    (data_offset) += x;                \
    (data) += x;                       \
})

#define MARK_ASSERT(cond) ({\
	if(!(cond))                        \
		return STREAM_ERR;             \
})

static inline uint32_t
mark_packet_fun(uint8_t *data, uint32_t pkt_sz) {
    uint16_t data_offset = 0;

    /* extract data from packet for marking */
    /* L2 rules */
    struct ethhdr *mac = (struct ethhdr *)data;
    SAFE_PKTDATA_OFFSET_ADV(data, data_offset, sizeof(struct ethhdr), pkt_sz);

    uint16_t ethertype = be16_to_cpu(mac->h_proto);

    switch(ethertype) {
        case ETH_P_ARP:
            return STREAM_1;
        case ETH_P_IP:
            break;
        default:
            return DEFAULT_CLASS;
    }
    /************/

    /* L3 rules */
    struct iphdr *iph = (struct iphdr *)data;
    SAFE_PKTDATA_OFFSET_ADV(data, data_offset, sizeof(struct iphdr), pkt_sz);

    /* skip non ipv4 payloads */
    if(iph->version != 4) {
        return DEFAULT_CLASS;
    }

    MARK_ASSERT((iph->ihl << 2) >= sizeof(struct iphdr));
    SAFE_PKTDATA_OFFSET_ADV(data, data_offset,
            (iph->ihl << 2) - sizeof(struct iphdr), pkt_sz);
    /************/

    /* L4 rules */
    void *transport_header = data;
    struct udphdr *udp_header = 0;
    struct tcphdr *tcp_header = 0;
    __attribute__((unused)) uint32_t src_ip = (unsigned int)iph->saddr;
    __attribute__((unused)) uint32_t dest_ip = (unsigned int)iph->daddr;
    __attribute__((unused)) uint16_t src_port = 0;
    __attribute__((unused)) uint16_t dest_port = 0;

    switch(iph->protocol) {
        case IPPROTO_UDP:
            udp_header = (struct udphdr *)transport_header;
            SAFE_PKTDATA_OFFSET_ADV(data, data_offset,
                        sizeof(struct udphdr), pkt_sz);
            src_port = (unsigned int)be16_to_cpu(udp_header->source);
            dest_port = (unsigned int)be16_to_cpu(udp_header->dest);
            break;
        case IPPROTO_TCP:
            tcp_header = (struct tcphdr *)transport_header;
            SAFE_PKTDATA_OFFSET_ADV(data, data_offset,
                        sizeof(struct tcphdr), pkt_sz);
            src_port = (unsigned int)be16_to_cpu(tcp_header->source);
            dest_port = (unsigned int)be16_to_cpu(tcp_header->dest);
            MARK_ASSERT((tcp_header->doff << 2) >= sizeof(struct tcphdr));
            SAFE_PKTDATA_OFFSET_ADV(data, data_offset,
                        (tcp_header->doff << 2) - sizeof(struct tcphdr), pkt_sz);
            break;
        case IPPROTO_ICMP:
            break;
        default:
            return DEFAULT_CLASS;
    }
    /************/

    /* L7/Payload rules */
    __attribute__((unused)) uint8_t *l7_payload = (uint8_t*)data;

    /* rule list */
    if(iph->protocol == IPPROTO_ICMP)
        return STREAM_1;

    if(iph->protocol == IPPROTO_UDP && dest_port == 53 &&
                find_substr(l7_payload, pkt_sz-data_offset, "cloudflare"))
        return STREAM_1;
    if(iph->protocol == IPPROTO_TCP && dest_port == 80 &&
                find_substr(l7_payload, pkt_sz-data_offset, "GET / HTTP/1.1"))
        return STREAM_2;


    return DEFAULT_CLASS;
}

#endif