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

/** MAC **/

/*
 *	IEEE 802.3 Ethernet magic constants.  The frame sizes omit the preamble
 *	and FCS/CRC (frame check sequence).
 */

#define ETH_ALEN		6		/* Octets in one ethernet addr	 */
#define ETH_TLEN		2		/* Octets in ethernet type field */
#define ETH_HLEN		14		/* Total octets in header.	 */
#define ETH_ZLEN		60		/* Min. octets in frame sans FCS */
#define ETH_DATA_LEN	1500	/* Max. octets in payload	 */
#define ETH_FRAME_LEN	1514	/* Max. octets in frame sans FCS */
#define ETH_FCS_LEN		4		/* Octets in the FCS		 */

#define ETH_MIN_MTU		68		/* Min IPv4 MTU per RFC791	*/
#define ETH_MAX_MTU		0xFFFFU	/* 65535, same as IP_MAX_MTU	*/

/*
 *	These are the defined Ethernet Protocol ID's.
 */

#define ETH_P_LOOP		0x0060		/* Ethernet Loopback packet	*/
#define ETH_P_PUP		0x0200		/* Xerox PUP packet		*/
#define ETH_P_PUPAT		0x0201		/* Xerox PUP Addr Trans packet	*/
#define ETH_P_TSN		0x22F0		/* TSN (IEEE 1722) packet	*/
#define ETH_P_ERSPAN2	0x22EB		/* ERSPAN version 2 (type III)	*/
#define ETH_P_IP		0x0800		/* Internet Protocol packet	*/
#define ETH_P_X25		0x0805		/* CCITT X.25			*/
#define ETH_P_ARP		0x0806		/* Address Resolution packet	*/
#define	ETH_P_BPQ		0x08FF		/* G8BPQ AX.25 Ethernet Packet	[ NOT AN OFFICIALLY REGISTERED ID ] */
#define ETH_P_IEEEPUP	0x0a00		/* Xerox IEEE802.3 PUP packet */
#define ETH_P_IEEEPUPAT	0x0a01		/* Xerox IEEE802.3 PUP Addr Trans packet */
#define ETH_P_BATMAN	0x4305		/* B.A.T.M.A.N.-Advanced packet [ NOT AN OFFICIALLY REGISTERED ID ] */
#define ETH_P_DEC       0x6000      /* DEC Assigned proto           */
#define ETH_P_DNA_DL    0x6001      /* DEC DNA Dump/Load            */
#define ETH_P_DNA_RC    0x6002      /* DEC DNA Remote Console       */
#define ETH_P_DNA_RT    0x6003      /* DEC DNA Routing              */
#define ETH_P_LAT       0x6004      /* DEC LAT                      */
#define ETH_P_DIAG      0x6005      /* DEC Diagnostics              */
#define ETH_P_CUST      0x6006      /* DEC Customer use             */
#define ETH_P_SCA       0x6007      /* DEC Systems Comms Arch       */
#define ETH_P_TEB		0x6558		/* Trans Ether Bridging		*/
#define ETH_P_RARP      0x8035		/* Reverse Addr Res packet	*/
#define ETH_P_ATALK		0x809B		/* Appletalk DDP		*/
#define ETH_P_AARP		0x80F3		/* Appletalk AARP		*/
#define ETH_P_8021Q		0x8100      /* 802.1Q VLAN Extended Header  */
#define ETH_P_ERSPAN	0x88BE		/* ERSPAN type II		*/
#define ETH_P_IPX		0x8137		/* IPX over DIX			*/
#define ETH_P_IPV6		0x86DD		/* IPv6 over bluebook		*/
#define ETH_P_PAUSE		0x8808		/* IEEE Pause frames. See 802.3 31B */
#define ETH_P_SLOW		0x8809		/* Slow Protocol. See 802.3ad 43B */
#define ETH_P_WCCP		0x883E		/* Web-cache coordination protocol
									 * defined in draft-wilson-wrec-wccp-v2-00.txt */
#define ETH_P_MPLS_UC	0x8847		/* MPLS Unicast traffic		*/
#define ETH_P_MPLS_MC	0x8848		/* MPLS Multicast traffic	*/
#define ETH_P_ATMMPOA	0x884c		/* MultiProtocol Over ATM	*/
#define ETH_P_PPP_DISC	0x8863		/* PPPoE discovery messages     */
#define ETH_P_PPP_SES	0x8864		/* PPPoE session messages	*/
#define ETH_P_LINK_CTL	0x886c		/* HPNA, wlan link local tunnel */
#define ETH_P_ATMFATE	0x8884		/* Frame-based ATM Transport
									 * over Ethernet
									 */
#define ETH_P_PAE		0x888E		/* Port Access Entity (IEEE 802.1X) */
#define ETH_P_AOE		0x88A2		/* ATA over Ethernet		*/
#define ETH_P_8021AD	0x88A8          /* 802.1ad Service VLAN		*/
#define ETH_P_802_EX1	0x88B5		/* 802.1 Local Experimental 1.  */
#define ETH_P_PREAUTH	0x88C7		/* 802.11 Preauthentication */
#define ETH_P_TIPC		0x88CA		/* TIPC 			*/
#define ETH_P_MACSEC	0x88E5		/* 802.1ae MACsec */
#define ETH_P_8021AH	0x88E7          /* 802.1ah Backbone Service Tag */
#define ETH_P_MVRP		0x88F5          /* 802.1Q MVRP                  */
#define ETH_P_1588		0x88F7		/* IEEE 1588 Timesync */
#define ETH_P_NCSI		0x88F8		/* NCSI protocol		*/
#define ETH_P_PRP		0x88FB		/* IEC 62439-3 PRP/HSRv0	*/
#define ETH_P_FCOE		0x8906		/* Fibre Channel over Ethernet  */
#define ETH_P_IBOE		0x8915		/* Infiniband over Ethernet	*/
#define ETH_P_TDLS		0x890D          /* TDLS */
#define ETH_P_FIP		0x8914		/* FCoE Initialization Protocol */
#define ETH_P_80221		0x8917		/* IEEE 802.21 Media Independent Handover Protocol */
#define ETH_P_HSR		0x892F		/* IEC 62439-3 HSRv1	*/
#define ETH_P_NSH		0x894F		/* Network Service Header */
#define ETH_P_LOOPBACK	0x9000		/* Ethernet loopback packet, per IEEE 802.3 */
#define ETH_P_QINQ1		0x9100		/* deprecated QinQ VLAN [ NOT AN OFFICIALLY REGISTERED ID ] */
#define ETH_P_QINQ2		0x9200		/* deprecated QinQ VLAN [ NOT AN OFFICIALLY REGISTERED ID ] */
#define ETH_P_QINQ3		0x9300		/* deprecated QinQ VLAN [ NOT AN OFFICIALLY REGISTERED ID ] */
#define ETH_P_EDSA		0xDADA		/* Ethertype DSA [ NOT AN OFFICIALLY REGISTERED ID ] */
#define ETH_P_IFE		0xED3E		/* ForCES inter-FE LFB type */
#define ETH_P_AF_IUCV   0xFBFB		/* IBM af_iucv [ NOT AN OFFICIALLY REGISTERED ID ] */

#define ETH_P_802_3_MIN	0x0600		/* If the value in the ethernet type is less than this value
									 * then the frame is Ethernet II. Else it is 802.3 */

#define ETH_P_802_3		0x0001		/* Dummy type for 802.3 frames  */
#define ETH_P_AX25		0x0002		/* Dummy protocol id for AX.25  */
#define ETH_P_ALL		0x0003		/* Every packet (be careful!!!) */
#define ETH_P_802_2		0x0004		/* 802.2 frames 		*/
#define ETH_P_SNAP		0x0005		/* Internal only		*/
#define ETH_P_DDCMP		0x0006		/* DEC DDCMP: Internal only     */
#define ETH_P_WAN_PPP	0x0007		/* Dummy type for WAN PPP frames*/
#define ETH_P_PPP_MP	0x0008		/* Dummy type for PPP MP frames */
#define ETH_P_LOCALTALK	0x0009		/* Localtalk pseudo type 	*/
#define ETH_P_CAN		0x000C		/* CAN: Controller Area Network */
#define ETH_P_CANFD		0x000D		/* CANFD: CAN flexible data rate*/
#define ETH_P_PPPTALK	0x0010		/* Dummy type for Atalk over PPP*/
#define ETH_P_TR_802_2	0x0011		/* 802.2 frames 		*/
#define ETH_P_MOBITEX	0x0015		/* Mobitex (kaz@cafe.net)	*/
#define ETH_P_CONTROL	0x0016		/* Card specific control frames */
#define ETH_P_IRDA		0x0017		/* Linux-IrDA			*/
#define ETH_P_ECONET	0x0018		/* Acorn Econet			*/
#define ETH_P_HDLC		0x0019		/* HDLC frames			*/
#define ETH_P_ARCNET	0x001A		/* 1A for ArcNet :-)            */
#define ETH_P_DSA		0x001B		/* Distributed Switch Arch.	*/
#define ETH_P_TRAILER	0x001C		/* Trailer switch tagging	*/
#define ETH_P_PHONET	0x00F5		/* Nokia Phonet frames          */
#define ETH_P_IEEE802154 0x00F6		/* IEEE802.15.4 frame		*/
#define ETH_P_CAIF		0x00F7		/* ST-Ericsson CAIF protocol	*/
#define ETH_P_XDSA		0x00F8		/* Multiplexed DSA protocol	*/
#define ETH_P_MAP		0x00F9		/* Qualcomm multiplexing and
									 * aggregation protocol */

struct ethhdr {
	unsigned char	h_dest[ETH_ALEN];	/* destination eth addr	*/
	unsigned char	h_source[ETH_ALEN];	/* source ether addr	*/
	__be16		h_proto;				/* packet type ID field	*/
} __attribute__((packed));


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