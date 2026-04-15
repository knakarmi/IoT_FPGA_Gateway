/* =============================================================================
 * IoT Gateway - Week 9
 * File   : packet_parser.h
 * Purpose: Parse raw Ethernet/IPv4/UDP/IoT frames into structured fields
 *
 * Packet structure supported:
 *   [ Ethernet header 14B ][ IPv4 header 20B ][ UDP header 8B ]
 *   [ IoT app header 8B  ][ Payload up to 1500B ]
 * =============================================================================
 */
#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include "xil_types.h"

// Return codes
#define PARSE_OK            0
#define PARSE_ERR_TOO_SHORT 1   /* buffer too short for headers */
#define PARSE_ERR_ETHERTYPE 2   /* unsupported EtherType (not IPv4) */
#define PARSE_ERR_PROTOCOL  3   /* unsupported IP protocol (not UDP) */
#define PARSE_ERR_BAD_LEN   4   /* declared length exceeds buffer */

 //Header sizes (bytes)
#define ETH_HEADER_LEN      14
#define IPV4_HEADER_LEN     20  /* assumes no IP options (IHL=5) */
#define UDP_HEADER_LEN      8
#define IOT_HEADER_LEN      8
#define MIN_PACKET_LEN      (ETH_HEADER_LEN + IPV4_HEADER_LEN + \
                             UDP_HEADER_LEN + IOT_HEADER_LEN)

// EtherType values
#define ETHERTYPE_IPV4      0x0800
#define ETHERTYPE_ARP       0x0806


// IP protocol numbers
#define IP_PROTO_ICMP       0x01
#define IP_PROTO_TCP        0x06
#define IP_PROTO_UDP        0x11

// IoT message types
#define IOT_MSG_RAW         0x01
#define IOT_MSG_SENSOR      0x02
#define IOT_MSG_COMMAND     0x03
#define IOT_MSG_ACK         0x04

/* ---------------------------------------------------------------------------
 * Ethernet header
 * 14 bytes: dst_mac(6) + src_mac(6) + ethertype(2)
 * --------------------------------------------------------------------------- */
typedef struct {
    u8  dst_mac[6];
    u8  src_mac[6];
    u16 ethertype;          /* big-endian */
} eth_header_t;

/* ---------------------------------------------------------------------------
 * IPv4 header (no options assumed, IHL=5)
 * 20 bytes
 * --------------------------------------------------------------------------- */
typedef struct {
    u8  version_ihl;        /* version=4, IHL=5 packed in one byte */
    u8  dscp_ecn;
    u16 total_length;       /* big-endian, includes IP header + payload */
    u16 identification;
    u16 flags_fragment;
    u8  ttl;
    u8  protocol;           /* 0x11 = UDP */
    u16 checksum;
    u8  src_ip[4];
    u8  dst_ip[4];
} ipv4_header_t;

/* ---------------------------------------------------------------------------
 * UDP header
 * 8 bytes
 * --------------------------------------------------------------------------- */
typedef struct {
    u16 src_port;           /* big-endian */
    u16 dst_port;           /* big-endian */
    u16 length;             /* big-endian, includes UDP header + payload */
    u16 checksum;
} udp_header_t;

/* ---------------------------------------------------------------------------
 * IoT application header
 * 8 bytes: device_id(2) + msg_type(1) + flags(1) + seq_num(2) + payload_len(2)
 * --------------------------------------------------------------------------- */
typedef struct {
    u16 device_id;          /* big-endian, unique device identifier */
    u8  msg_type;           /* IOT_MSG_* */
    u8  flags;              /* reserved, should be 0 */
    u16 seq_num;            /* big-endian, packet sequence number */
    u16 payload_len;        /* big-endian, payload bytes that follow */
} iot_header_t;

// Complete parsed packet structure
typedef struct {
    eth_header_t    eth;
    ipv4_header_t   ipv4;
    udp_header_t    udp;
    iot_header_t    iot;

    const u8       *payload;        /* pointer into original buffer */
    u16             payload_len;    /* actual payload bytes available */

    u32             total_len;      /* total packet length parsed */
    int             parse_result;   /* PARSE_OK or PARSE_ERR_* */
} iot_packet_t;

int parse_packet(const u8 *buf, u32 len, iot_packet_t *pkt);

void print_packet_info(const iot_packet_t *pkt);

const char *get_msg_type_str(u8 msg_type);

#endif /* PACKET_PARSER_H */
