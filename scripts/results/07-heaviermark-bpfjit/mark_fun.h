#ifndef MARK_FUN_H_
#define MARK_FUN_H_

// static inline int
// find_substr(const uint8_t *buf, uint16_t buf_len, const char *pattern) {
//     int plen = 0;
//     while(pattern[plen] == '\0')
//         plen++;

//     if(plen == 0 || buf_len < plen)
//         return -1;

//     int i,j;
//     for(i = 0; i <= buf_len - plen; i++) {
//         for(j = 0; j < plen; j++)
//             if(buf[i+j] != pattern[j])
//                 break;
//         if(j == plen) // found full match
//             return 1;
//     }

//     return -1;
// }

static inline uint32_t
bpf_strlen(const char *str) {
	const char *s;
	for (s = str; *s; ++s);
	return (s - str);
}

static inline int
find_prefix(const uint8_t *buf, uint16_t buf_len, const char *prefix) {
    uint32_t prefix_len = bpf_strlen(prefix);
    if(prefix_len == 0 || prefix_len > buf_len)
        return 0;

    for(uint32_t i=0; i<prefix_len; i++)
        if(buf[i] != prefix[i])
            return 0;

    return 1;
}

#define STREAM_A1        0;
#define STREAM_A2        1;
#define STREAM_A3        2;
#define STREAM_A4        3;
#define STREAM_B1        4;
#define STREAM_B2        5;
#define STREAM_B3        6;
#define STREAM_B4        7;
#define STREAM_C1        8;
#define STREAM_C2        9;
#define STREAM_C3        10;
#define STREAM_C4        11;
#define STREAM_ERR       12;
#define DEFAULT_CLASS    13;
#define STREAM_BY_CLASS(cl,strm) (cl*4+strm)

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

#define IPADDR(a1,a2,a3,a4) (a1 << 24 | a2 << 16 | a3 << 8 | a4)

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
            return STREAM_A1;
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
    __attribute__((unused)) uint32_t payload_size = pkt_sz-data_offset;

    /* Section 0: high priority */
    /* ICMP */
    if(iph->protocol == IPPROTO_ICMP)
        return STREAM_A1;

    /* Google DNS */
    if(iph->protocol == IPPROTO_UDP && dest_port == 53 &&
        (dest_ip == cpu_to_be32(IPADDR(8,8,8,8)) || dest_ip == IPADDR(8,8,4,4)))
        return STREAM_A1;

    /* Other DNS */
    if(iph->protocol == IPPROTO_UDP && dest_port == 53)
        return STREAM_A2;

    /* Split into class A and B based on ip address and tos */
    uint8_t ipclass = 2;
    if(iph->tos == 0x0c || iph->tos == 0xb8 ||
            (be32_to_cpu(dest_ip) & IPADDR(255,255,255,0)) == IPADDR(172,16,139,0))
        ipclass = 0;
    else if((be32_to_cpu(dest_ip) & IPADDR(255,255,255,0)) == IPADDR(172,16,128,0))
        ipclass = 1;

    /* real time VOIP/AUDIO traffic */
    if(iph->protocol == IPPROTO_UDP && dest_port == 1853)
        return STREAM_BY_CLASS(ipclass, 0);

    /* SSH/Telnet/RDP sessions */
    if(iph->protocol == IPPROTO_TCP && (dest_port == 22 || dest_port == 23 || dest_port == 3389))
        return STREAM_BY_CLASS(ipclass, 1);

    /* small TCP SYN and ACK packets */
    if(iph->protocol == IPPROTO_TCP && (tcp_header->syn || tcp_header->ack) && payload_size < 200)
        return STREAM_BY_CLASS(ipclass, 1);

    /* NFS server */
    if((iph->protocol == IPPROTO_TCP || iph->protocol == IPPROTO_UDP) &&
        (dest_port == 111 || dest_port == 2049))
        return STREAM_BY_CLASS(ipclass, 2);

    /* FTP/SFTP sessions */
    if(iph->protocol == IPPROTO_TCP && (dest_port == 20 || dest_port == 21 || dest_port == 69))
        return STREAM_BY_CLASS(ipclass, 2);

    /* HTTP/HTTPS init + payload */
    if(iph->protocol == IPPROTO_TCP && (dest_port == 80 || dest_port == 443))
        return STREAM_BY_CLASS(ipclass, 3);

    /* Low priority targets */
    /* HTTP init on other ports (we cannot track connections yet for payload marking) */
    char http_prefix[] = "GET / HTTP/1.1";
    if(iph->protocol == IPPROTO_TCP &&
                find_prefix(l7_payload, payload_size, http_prefix))
        return STREAM_B1;

    /* junk */
    return DEFAULT_CLASS;
}

#endif