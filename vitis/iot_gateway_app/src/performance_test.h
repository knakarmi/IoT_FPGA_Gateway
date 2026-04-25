/* =============================================================================
 * IoT Gateway - Week 11
 * File   : performance_test.h
 * Purpose: Performance evaluation test suite
 * =============================================================================
 */
#ifndef PERFORMANCE_TEST_H
#define PERFORMANCE_TEST_H

#include "xil_types.h"

/* Return codes */
#define PERF_PASS   0
#define PERF_FAIL   1

/* Reset interval (packets between AES pipeline resets during long-running
 * measurements). A periodic reset keeps the core in a clean state. */
#define PERF_RESET_EVERY      30

/* Number of packets per measurement point */
#define PERF_THROUGHPUT_N     30   /* per packet size */
#define PERF_LATENCY_N       100   /* for latency histogram */
#define PERF_BREAKDOWN_N      20   /* per-stage timing samples */
#define PERF_JITTER_BATCHES   10   /* batches for throughput jitter test */
#define PERF_JITTER_BATCH_N   30   /* packets per jitter batch */

/* Packet size sweep */
#define PERF_NUM_SIZES        6
extern const u32 perf_packet_sizes[PERF_NUM_SIZES];

/* Entry point */
int run_performance_tests(const u8 *key, const u8 *iv);

#endif /* PERFORMANCE_TEST_H */
