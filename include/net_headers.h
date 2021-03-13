/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

/** Endianness utils **/
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint32_t __u64;

typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;

typedef __u16 __sum16;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#define le16_to_cpu(x) x
#define le32_to_cpu(x) x
#define le64_to_cpu(x) x

#define cpu_to_le16(x) x
#define cpu_to_le32(x) x
#define cpu_to_le64(x) x

__u16	be16_to_cpu(const __be16 x) { return __builtin_bswap16(x); }
__u32	be32_to_cpu(const __be32 x) { return __builtin_bswap32(x); }
__u64	be64_to_cpu(const __be64 x) { return __builtin_bswap64(x); }

__be16	cpu_to_be16(const __u16 x) { return __builtin_bswap16(x); }
__be32	cpu_to_be32(const __u32 x) { return __builtin_bswap32(x); }
__be64	cpu_to_be64(const __u64 x) { return __builtin_bswap64(x); }

#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

__u16	le16_to_cpu(const __le16 x) { return __builtin_bswap16(x); }
__u32	le32_to_cpu(const __le32 x) { return __builtin_bswap32(x); }
__u64	le64_to_cpu(const __le64 x) { return __builtin_bswap64(x); }

__le16	cpu_to_le16(const __u16 x) { return __builtin_bswap16(x); }
__le32	cpu_to_le32(const __u32 x) { return __builtin_bswap32(x); }
__le64	cpu_to_le64(const __u64 x) { return __builtin_bswap64(x); }

#define be16_to_cpu(x) x
#define be32_to_cpu(x) x
#define be64_to_cpu(x) x

#define cpu_to_be16(x) x
#define cpu_to_be32(x) x
#define cpu_to_be64(x) x

#else
#error	"Compiler Endianness Macro required"
#endif

/** IP **/
#define IPPROTO_IP		0 	/* Dummy protocol for TCP */
#define IPPROTO_ICMP	1	/* Internet Control Message Protocol */
#define IPPROTO_IGMP	2	/* Internet Group Management Protocol */
#define IPPROTO_IPIP	4	/* IPIP tunnels (older KA9Q tunnels use 94 */
#define IPPROTO_TCP		6	/* Transmission Control Protocol */
#define IPPROTO_EGP		8	/* Exterior Gateway Protocol */
#define IPPROTO_PUP		12	/* PUP protocol */
#define IPPROTO_UDP		17	/* User Datagram Protocol */
#define IPPROTO_IDP		22	/* XNS IDP protocol */
#define IPPROTO_TP		29	/* SO Transport Protocol Class 4 */
#define IPPROTO_DCCP	33	/* Datagram Congestion Control Protocol */
#define IPPROTO_IPV6	41	/* IPv6-in-IPv4 tunnelling */
#define IPPROTO_RSVP	46	/* RSVP Protocol */
#define IPPROTO_GRE		47	/* Cisco GRE tunnels (rfc 1701,1702 */
#define IPPROTO_ESP		50	/* Encapsulation Security Payload protocol */
#define IPPROTO_AH		51	/* Authentication Header protocol */
#define IPPROTO_MTP		92	/* Multicast Transport Protocol */
#define IPPROTO_BEETPH	94	/* IP option pseudo header for BEET */
#define IPPROTO_ENCAP	98	/* Encapsulation Header */
#define IPPROTO_PIM		103	/* Protocol Independent Multicast */
#define IPPROTO_COMP	108	/* Compression Header Protocol */
#define IPPROTO_SCTP	132	/* Stream Control Transport Protocol */
#define IPPROTO_UDPLITE	136	/* UDP-Lite (RFC 3828 */
#define IPPROTO_MPLS	137	/* MPLS in IP (RFC 4023 */
#define IPPROTO_RAW		255	/* Raw IP packets */

struct iphdr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	__u8	ihl:4,
		version:4;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	__u8	version:4,
  		ihl:4;
#endif
	__u8	tos;
	__be16	tot_len;
	__be16	id;
	__be16	frag_off;
	__u8	ttl;
	__u8	protocol;
	__sum16	check;
	__be32	saddr;
	__be32	daddr;
	/*The options start here. */
};

/** UDP **/
struct udphdr {
	__be16	source;
	__be16	dest;
	__be16	len;
	__sum16	check;
};

/** TCP **/
struct tcphdr {
	__be16	source;
	__be16	dest;
	__be32	seq;
	__be32	ack_seq;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	__u16	res1:4,
		doff:4,
		fin:1,
		syn:1,
		rst:1,
		psh:1,
		ack:1,
		urg:1,
		ece:1,
		cwr:1;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	__u16	doff:4,
		res1:4,
		cwr:1,
		ece:1,
		urg:1,
		ack:1,
		psh:1,
		rst:1,
		syn:1,
		fin:1;
#endif	
	__be16	window;
	__sum16	check;
	__be16	urg_ptr;
};