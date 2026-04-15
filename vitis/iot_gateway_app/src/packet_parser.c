/* =============================================================================
 * IoT Gateway - Week 9
 * File   : packet_parser.c
 * Purpose: Parse raw Ethernet/IPv4/UDP/IoT frames into structured fields
 *
 * Parsing approach:
 *   All multi-byte fields in network packets are big-endian (network byte
 *   order). The ARM Cortex-A9 is little-endian, so we manually reconstruct
 *   16-bit and 32-bit values from individual bytes rather than casting
 *   pointers directly (which would give wrong byte order and also risks
 *   unaligned memory access faults).
 *
 *   For example, to read a big-endian u16 at bytes [i] and [i+1]:
 *     value = ((u16)buf[i] << 8) | buf[i+1]
 *   This is always correct regardless of CPU endianness.
 * =============================================================================
 */

#include "packet_parser.h"
#include "xil_printf.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * be16 - read a big-endian 16-bit value from a byte buffer at offset
 * --------------------------------------------------------------------------- */
static inline u16 be16(const u8 *buf, u32 offset)
{
    return ((u16)buf[offset] << 8) | (u16)buf[offset + 1];
}

/* ---------------------------------------------------------------------------
 * parse_packet
 * Walk through each protocol layer and extract fields.
 * --------------------------------------------------------------------------- */
int parse_packet(const u8 *buf, u32 len, iot_packet_t *pkt)
{
    u32 offset = 0;

    /* Clear the output structure */
    memset(pkt, 0, sizeof(iot_packet_t));
    pkt->parse_result = PARSE_OK;
    pkt->total_len    = len;

    /* ------------------------------------------------------------------
     * Layer 1: Ethernet header (14 bytes)
     *
     * Layout: [ dst_mac 6B ][ src_mac 6B ][ ethertype 2B ]
     * ------------------------------------------------------------------ */
    if (len < ETH_HEADER_LEN) {
        pkt->parse_result = PARSE_ERR_TOO_SHORT;
        return PARSE_ERR_TOO_SHORT;
    }

    /* Copy destination and source MAC addresses (6 bytes each) */
    memcpy(pkt->eth.dst_mac, buf + offset, 6);
    offset += 6;
    memcpy(pkt->eth.src_mac, buf + offset, 6);
    offset += 6;

    /* EtherType: 2 bytes, big-endian
     * 0x0800 = IPv4, 0x0806 = ARP, 0x86DD = IPv6
     * Only support IPv4 in this gateway */
    pkt->eth.ethertype = be16(buf, offset);
    offset += 2;

    if (pkt->eth.ethertype != ETHERTYPE_IPV4) {
        pkt->parse_result = PARSE_ERR_ETHERTYPE;
        return PARSE_ERR_ETHERTYPE;
    }

    /* ------------------------------------------------------------------
     * Layer 2: IPv4 header (20 bytes, assuming no options)
     *
     * Layout:
     *   [ ver+IHL 1B ][ DSCP+ECN 1B ][ total_len 2B ][ ID 2B ]
     *   [ flags+frag 2B ][ TTL 1B ][ proto 1B ][ checksum 2B ]
     *   [ src_ip 4B ][ dst_ip 4B ]
     *
     * version_ihl: upper nibble = IP version (should be 4)
     *              lower nibble = IHL (header length in 32-bit words)
     *              IHL=5 means 5*4=20 bytes, no options
     *
     * total_length: length of entire IP datagram including IP header
     *               so actual payload after IP header = total_length - IHL*4
     * ------------------------------------------------------------------ */
    if (len < ETH_HEADER_LEN + IPV4_HEADER_LEN) {
        pkt->parse_result = PARSE_ERR_TOO_SHORT;
        return PARSE_ERR_TOO_SHORT;
    }

    pkt->ipv4.version_ihl  = buf[offset++];
    pkt->ipv4.dscp_ecn     = buf[offset++];
    pkt->ipv4.total_length = be16(buf, offset); offset += 2;
    pkt->ipv4.identification = be16(buf, offset); offset += 2;
    pkt->ipv4.flags_fragment = be16(buf, offset); offset += 2;
    pkt->ipv4.ttl           = buf[offset++];
    pkt->ipv4.protocol      = buf[offset++];
    pkt->ipv4.checksum      = be16(buf, offset); offset += 2;
    memcpy(pkt->ipv4.src_ip, buf + offset, 4); offset += 4;
    memcpy(pkt->ipv4.dst_ip, buf + offset, 4); offset += 4;

    /* Only handle UDP */
    if (pkt->ipv4.protocol != IP_PROTO_UDP) {
        pkt->parse_result = PARSE_ERR_PROTOCOL;
        return PARSE_ERR_PROTOCOL;
    }

    /* ------------------------------------------------------------------
     * Layer 3: UDP header (8 bytes)
     *
     * Layout: [ src_port 2B ][ dst_port 2B ][ length 2B ][ checksum 2B ]
     *
     * UDP length includes the 8-byte UDP header itself, so actual
     * application data = udp.length - 8
     *
     * Well-known ports relevant to IoT:
     *   1883 = MQTT (unencrypted)
     *   8883 = MQTT over TLS
     *   5683 = CoAP
     * ------------------------------------------------------------------ */
    if (len < (u32)(ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN)) {
        pkt->parse_result = PARSE_ERR_TOO_SHORT;
        return PARSE_ERR_TOO_SHORT;
    }

    pkt->udp.src_port = be16(buf, offset); offset += 2;
    pkt->udp.dst_port = be16(buf, offset); offset += 2;
    pkt->udp.length   = be16(buf, offset); offset += 2;
    pkt->udp.checksum = be16(buf, offset); offset += 2;

    /* ------------------------------------------------------------------
     * Layer 4: IoT application header (8 bytes)
     *
     * Layout:
     *   [ device_id 2B ][ msg_type 1B ][ flags 1B ]
     *   [ seq_num 2B ][ payload_len 2B ]
     *
     * device_id: unique 16-bit identifier for each IoT sensor/actuator
     * msg_type:  what kind of message this is (raw data, sensor reading,
     *            command, acknowledgment)
     * seq_num:   monotonically increasing counter per device, used to
     *            detect dropped or reordered packets
     * payload_len: how many bytes of application data follow this header
     * ------------------------------------------------------------------ */
    if (len < (u32)(ETH_HEADER_LEN + IPV4_HEADER_LEN +
                    UDP_HEADER_LEN + IOT_HEADER_LEN)) {
        pkt->parse_result = PARSE_ERR_TOO_SHORT;
        return PARSE_ERR_TOO_SHORT;
    }

    pkt->iot.device_id   = be16(buf, offset); offset += 2;
    pkt->iot.msg_type    = buf[offset++];
    pkt->iot.flags       = buf[offset++];
    pkt->iot.seq_num     = be16(buf, offset); offset += 2;
    pkt->iot.payload_len = be16(buf, offset); offset += 2;

    /* ------------------------------------------------------------------
     * Payload
     *
     * Just a pointer into the original buffer.
      ------------------------------------------------------------------ */
    pkt->payload = buf + offset;

    /* Bytes remaining in buffer after all headers */
    u32 remaining = (len > offset) ? (len - offset) : 0;

    /* Use whichever is smaller to avoid reading past buffer */
    pkt->payload_len = (pkt->iot.payload_len < (u16)remaining)
                       ? pkt->iot.payload_len
                       : (u16)remaining;

    /* Sanity check: declared payload_len should not exceed buffer */
    if (pkt->iot.payload_len > remaining) {
        xil_printf("PARSER: WARNING - declared payload_len=%d but only "
                   "%lu bytes remain\r\n",
                   (int)pkt->iot.payload_len, remaining);
        pkt->parse_result = PARSE_ERR_BAD_LEN;
        /* No return error */
    }

    return PARSE_OK;
}

/* ---------------------------------------------------------------------------
 * get_msg_type_str
 * Convert a numeric msg_type to a human-readable string.
 * makes output easier to read than raw hex values
 * --------------------------------------------------------------------------- */
const char *get_msg_type_str(u8 msg_type)
{
    switch (msg_type) {
        case IOT_MSG_RAW:     return "RAW";
        case IOT_MSG_SENSOR:  return "SENSOR";
        case IOT_MSG_COMMAND: return "COMMAND";
        case IOT_MSG_ACK:     return "ACK";
        default:              return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * print_packet_info
 * Print all parsed fields to UART in a readable format.
 *
 * Function to call after parsing to log/inspect a packet before it goes into the encryption pipeline.
 * --------------------------------------------------------------------------- */
void print_packet_info(const iot_packet_t *pkt)
{
    u32 i;

    xil_printf("------ Parsed Packet (%lu bytes) ------\r\n",
               pkt->total_len);

    /* Ethernet layer */
    xil_printf("[ETH]  dst=%02X:%02X:%02X:%02X:%02X:%02X  "
               "src=%02X:%02X:%02X:%02X:%02X:%02X  "
               "type=0x%04X\r\n",
               pkt->eth.dst_mac[0], pkt->eth.dst_mac[1],
               pkt->eth.dst_mac[2], pkt->eth.dst_mac[3],
               pkt->eth.dst_mac[4], pkt->eth.dst_mac[5],
               pkt->eth.src_mac[0], pkt->eth.src_mac[1],
               pkt->eth.src_mac[2], pkt->eth.src_mac[3],
               pkt->eth.src_mac[4], pkt->eth.src_mac[5],
               (unsigned int)pkt->eth.ethertype);

    /* IPv4 layer */
    xil_printf("[IPv4] src=%d.%d.%d.%d  dst=%d.%d.%d.%d  "
               "proto=0x%02X  TTL=%d  len=%d\r\n",
               pkt->ipv4.src_ip[0], pkt->ipv4.src_ip[1],
               pkt->ipv4.src_ip[2], pkt->ipv4.src_ip[3],
               pkt->ipv4.dst_ip[0], pkt->ipv4.dst_ip[1],
               pkt->ipv4.dst_ip[2], pkt->ipv4.dst_ip[3],
               (unsigned int)pkt->ipv4.protocol,
               (int)pkt->ipv4.ttl,
               (int)pkt->ipv4.total_length);

    /* UDP layer */
    xil_printf("[UDP]  src_port=%d  dst_port=%d  len=%d\r\n",
               (int)pkt->udp.src_port,
               (int)pkt->udp.dst_port,
               (int)pkt->udp.length);

    /* IoT application layer */
    xil_printf("[IoT]  device_id=0x%04X  msg_type=%s(0x%02X)  "
               "seq=%d  payload_len=%d  flags=0x%02X\r\n",
               (unsigned int)pkt->iot.device_id,
               get_msg_type_str(pkt->iot.msg_type),
               (unsigned int)pkt->iot.msg_type,
               (int)pkt->iot.seq_num,
               (int)pkt->iot.payload_len,
               (unsigned int)pkt->iot.flags);

    /* Payload bytes */
    if (pkt->payload_len > 0) {
        xil_printf("[PAY]  %d bytes: ", (int)pkt->payload_len);
        for (i = 0; i < pkt->payload_len && i < 16; i++) {
            xil_printf("%02X ", pkt->payload[i]);
        }
        if (pkt->payload_len > 16) {
            xil_printf("...");
        }
        xil_printf("\r\n");
    }

    xil_printf("---------------------------------------\r\n");
}
