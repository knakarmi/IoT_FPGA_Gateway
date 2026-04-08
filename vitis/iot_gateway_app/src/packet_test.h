/* =============================================================================
 * IoT Gateway - Week 7
 * File   : packet_test.h
 * Fix v2 : Cleaned up - interrupt_handler.h moved to .c file
 * =============================================================================
 */
#ifndef PACKET_TEST_H
#define PACKET_TEST_H

#include "xil_types.h"

int run_packet_test(const u8 *pkt, u32 len);

#endif /* PACKET_TEST_H */
