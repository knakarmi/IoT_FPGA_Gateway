/* =============================================================================
 * IoT Gateway - Week 11
 * File   : performance_test.c
 * Purpose: Performance evaluation - throughput, latency, resource utilization
 *
 * Test scenarios:
 *   A - Throughput sweep: 64, 128, 256, 512, 1024, 1500 byte packets
 *   B - Latency histogram: 100 back-to-back 64-byte transfers with stats
 *   C - Latency breakdown: per-stage timing (rearm, TX, AES, RX, recv)
 *   D - Throughput jitter: batch-to-batch variance analysis
 *
 * A periodic AES pipeline reset (pipe_reset + key/IV reload) is issued every
 * PERF_RESET_EVERY packets to maintain clean pipeline state during long runs.
 * =============================================================================
 */

#include "performance_test.h"
#include "dma_handler.h"
#include "aes_key_loader.h"
#include "timing.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Packet size sweep
 * --------------------------------------------------------------------------- */
const u32 perf_packet_sizes[PERF_NUM_SIZES] = {
    64, 128, 256, 512, 1024, 1500
};

/* ---------------------------------------------------------------------------
 * Persistent results struct - populated by each test, printed at end
 * --------------------------------------------------------------------------- */
typedef struct {
    /* Test A - throughput sweep */
    u32 thrpt_mbps_x100[PERF_NUM_SIZES]; /* Mbps * 100 for each packet size */
    u32 thrpt_peak_mbps_x100;            /* best overall Mbps*100 */
    u32 thrpt_peak_size;                 /* packet size at peak */

    /* Test B - latency */
    u32 lat_min_us;
    u32 lat_max_us;
    u32 lat_mean_us;
    u32 lat_stdev_us;
    u32 lat_spread_us;

    /* Test C - latency breakdown */
    u32 brkdn_s1_us, brkdn_s2_us, brkdn_s3_us, brkdn_s4_us, brkdn_s5_us;
    u32 brkdn_total_us;
    u32 brkdn_dominant_idx;   /* 0..4 into stage name table */
    u32 brkdn_dominant_pct;

    /* Test D - throughput jitter */
    u32 jitter_min_us;
    u32 jitter_max_us;
    u32 jitter_mean_us;
    u32 jitter_stdev_us;
    u32 jitter_cov_ppm;       /* CoV in parts per million */
} perf_results_t;

static perf_results_t g_results;

static const char *stage_names_short[5] = {
    "S1 rearm",   "S2 TX+IOC",  "S3 EOP",
    "S4 RX wait", "S5 recv"
};

/* ---------------------------------------------------------------------------
 * Test packet buffer
 * --------------------------------------------------------------------------- */
#define PERF_MAX_PKT_SIZE 1500
static u8 perf_pkt_buf[PERF_MAX_PKT_SIZE] __attribute__((aligned(32)));
static u8 perf_rx_buf[2048]               __attribute__((aligned(32)));

static void init_test_packet(u32 len)
{
    u32 i;
    for (i = 0; i < len; i++) {
        perf_pkt_buf[i] = (u8)(0xA0 + (i & 0x3F));
    }
    if (len >= 4) {
        perf_pkt_buf[0] = 0xDE;
        perf_pkt_buf[1] = 0xAD;
        perf_pkt_buf[2] = 0xBE;
        perf_pkt_buf[3] = 0xEF;
    }
}

extern void aes_start_packet(void);
extern void aes_end_packet(void);

/* ---------------------------------------------------------------------------
 * aes_reset_and_reload - full AES pipeline reset + key/IV reload
 * --------------------------------------------------------------------------- */
static int aes_reset_and_reload(const u8 *key, const u8 *iv)
{
    return aes_load_key(key, iv);
}

/* ---------------------------------------------------------------------------
 * encrypt_one_packet - silent single-packet pipeline pass
 * --------------------------------------------------------------------------- */
static int encrypt_one_packet(const u8 *pkt, u32 len,
                               u8 *out_buf, u32 *out_len)
{
    int status;
    u32 timeout;

    g_dma_error = 0;

    status = dma_rearm_rx();
    if (status != XST_SUCCESS) return 0;

    aes_start_packet();

    status = dma_send_packet((u8 *)pkt, len);
    if (status != XST_SUCCESS) { aes_end_packet(); return 0; }

    timeout = 0;
    while (!g_dma_tx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 2000000) { aes_end_packet(); return 0; }
    }
    if (g_dma_error) { g_dma_error = 0; aes_end_packet(); return 0; }

    usleep(10);
    aes_end_packet();

    timeout = 0;
    while (!g_dma_rx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 500000) {
            if (dma_poll_rx(1500000) != XST_SUCCESS) return 0;
            break;
        }
    }
    if (g_dma_error) { g_dma_error = 0; return 0; }

    status = dma_recv_packet(out_buf, out_len);
    if (status != XST_SUCCESS) return 0;

    u32 j;
    for (j = 0; j < 16 && j < *out_len; j++) {
        if (out_buf[j] != 0) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Test A: Throughput sweep
 * --------------------------------------------------------------------------- */
static int test_throughput_sweep(const u8 *key, const u8 *iv)
{
    u32 sz_idx, i;
    u32 t_start, t_end, cycles, total_us;
    int failures = 0;

    g_results.thrpt_peak_mbps_x100 = 0;
    g_results.thrpt_peak_size = 0;

    xil_printf("\r\n[PERF A] Throughput sweep\r\n");
    xil_printf("---------------------------------------\r\n");
    xil_printf(" Size(B) | Time(us) | Thrpt(Mbps) | Errs\r\n");
    xil_printf("---------------------------------------\r\n");

    for (sz_idx = 0; sz_idx < PERF_NUM_SIZES; sz_idx++) {
        u32 sz = perf_packet_sizes[sz_idx];
        u32 errs_this_size = 0;

        if (aes_reset_and_reload(key, iv) != XST_SUCCESS) {
            xil_printf(" %7lu | RESET FAILED\r\n", sz);
            failures++;
            g_results.thrpt_mbps_x100[sz_idx] = 0;
            continue;
        }

        init_test_packet(sz);

        t_start = timing_now();
        for (i = 0; i < PERF_THROUGHPUT_N; i++) {
            u32 rx_len = 0;
            if (i > 0 && (i % PERF_RESET_EVERY) == 0) {
                aes_reset_and_reload(key, iv);
            }
            if (!encrypt_one_packet(perf_pkt_buf, sz,
                                     perf_rx_buf, &rx_len)) {
                errs_this_size++;
            }
        }
        t_end = timing_now();

        cycles = t_end - t_start;
        total_us = timing_cycles_to_us(cycles);

        u64 total_bits = (u64)PERF_THROUGHPUT_N * sz * 8;
        u32 mbps_x100 = (u32)((total_bits * 100) / (total_us ? total_us : 1));

        g_results.thrpt_mbps_x100[sz_idx] = mbps_x100;
        if (mbps_x100 > g_results.thrpt_peak_mbps_x100) {
            g_results.thrpt_peak_mbps_x100 = mbps_x100;
            g_results.thrpt_peak_size = sz;
        }

        xil_printf(" %7lu | %8lu | %5lu.%02lu   | %4lu\r\n",
                   sz,
                   (unsigned long)total_us,
                   (unsigned long)(mbps_x100 / 100),
                   (unsigned long)(mbps_x100 % 100),
                   (unsigned long)errs_this_size);

        failures += errs_this_size;
    }

    xil_printf("---------------------------------------\r\n");
    if (failures == 0) {
        xil_printf(" PASS: all packet sizes encrypted cleanly\r\n");
        return PERF_PASS;
    }
    u32 total = PERF_THROUGHPUT_N * PERF_NUM_SIZES;
    if (failures < (int)(total / 20)) {
        xil_printf(" PASS: %d/%lu transfers with %d transient errors\r\n",
                   total - failures, (unsigned long)total, failures);
        return PERF_PASS;
    }
    xil_printf(" FAIL: %d total errors\r\n", failures);
    return PERF_FAIL;
}

/* ---------------------------------------------------------------------------
 * Test B: Latency histogram
 * --------------------------------------------------------------------------- */
#define HIST_BUCKET_USEC    1
#define HIST_NUM_BUCKETS   20
#define HIST_BAR_CAP       40

static int test_latency_histogram(const u8 *key, const u8 *iv)
{
    u32 i;
    u32 latencies[PERF_LATENCY_N];
    u32 hist[HIST_NUM_BUCKETS];
    int errs = 0;

    xil_printf("\r\n[PERF B] Latency histogram (%d packets, 64 bytes)\r\n",
               PERF_LATENCY_N);

    if (aes_reset_and_reload(key, iv) != XST_SUCCESS) {
        xil_printf(" FAIL: reset failed\r\n");
        return PERF_FAIL;
    }

    init_test_packet(64);

    for (i = 0; i < PERF_LATENCY_N; i++) {
        u32 t0, t1, rx_len;
        if (i > 0 && (i % PERF_RESET_EVERY) == 0) {
            aes_reset_and_reload(key, iv);
        }
        t0 = timing_now();
        if (!encrypt_one_packet(perf_pkt_buf, 64,
                                 perf_rx_buf, &rx_len)) {
            errs++;
            latencies[i] = 0;
            continue;
        }
        t1 = timing_now();
        latencies[i] = timing_cycles_to_us(t1 - t0);
    }

    u32 min_us = 0xFFFFFFFFu;
    u32 max_us = 0;
    u64 sum_us = 0;
    u32 valid_count = 0;
    for (i = 0; i < PERF_LATENCY_N; i++) {
        if (latencies[i] == 0) continue;
        if (latencies[i] < min_us) min_us = latencies[i];
        if (latencies[i] > max_us) max_us = latencies[i];
        sum_us += latencies[i];
        valid_count++;
    }

    if (valid_count == 0) {
        xil_printf(" FAIL: no valid samples\r\n");
        return PERF_FAIL;
    }

    u32 mean_us = (u32)(sum_us / valid_count);
    u64 var_sum = 0;
    for (i = 0; i < PERF_LATENCY_N; i++) {
        if (latencies[i] == 0) continue;
        s32 d = (s32)latencies[i] - (s32)mean_us;
        var_sum += (u64)(d * d);
    }
    u32 variance = (u32)(var_sum / valid_count);
    u32 stdev = 0;
    while ((stdev + 1) * (stdev + 1) <= variance) stdev++;

    /* Persist to summary */
    g_results.lat_min_us    = min_us;
    g_results.lat_max_us    = max_us;
    g_results.lat_mean_us   = mean_us;
    g_results.lat_stdev_us  = stdev;
    g_results.lat_spread_us = max_us - min_us;

    /* Histogram - auto-center on min */
    u32 hist_base = min_us;
    for (i = 0; i < HIST_NUM_BUCKETS; i++) hist[i] = 0;
    for (i = 0; i < PERF_LATENCY_N; i++) {
        if (latencies[i] == 0) continue;
        u32 offset = latencies[i] - hist_base;
        u32 bucket = offset / HIST_BUCKET_USEC;
        if (bucket >= HIST_NUM_BUCKETS) bucket = HIST_NUM_BUCKETS - 1;
        hist[bucket]++;
    }

    xil_printf(" Valid samples : %lu / %d  (errors: %d)\r\n",
               (unsigned long)valid_count, PERF_LATENCY_N, errs);
    xil_printf(" Min    : %5lu us\r\n", (unsigned long)min_us);
    xil_printf(" Max    : %5lu us\r\n", (unsigned long)max_us);
    xil_printf(" Mean   : %5lu us\r\n", (unsigned long)mean_us);
    xil_printf(" Stdev  : %5lu us\r\n", (unsigned long)stdev);
    xil_printf(" Spread : %5lu us  (max - min)\r\n",
               (unsigned long)(max_us - min_us));

    xil_printf("\r\n Histogram (1 us buckets, each '#' = 1 sample):\r\n");
    for (i = 0; i < HIST_NUM_BUCKETS; i++) {
        if (hist[i] == 0) continue;
        u32 bucket_start = hist_base + i * HIST_BUCKET_USEC;
        xil_printf("  [%5lu us] %3lu |",
                   (unsigned long)bucket_start,
                   (unsigned long)hist[i]);
        u32 bar = hist[i];
        if (bar > HIST_BAR_CAP) bar = HIST_BAR_CAP;
        u32 k;
        for (k = 0; k < bar; k++) xil_printf("#");
        if (hist[i] > HIST_BAR_CAP) xil_printf("...");
        xil_printf("\r\n");
    }

    if (errs < PERF_LATENCY_N / 20) {
        xil_printf("\r\n PASS: latency statistics collected\r\n");
        return PERF_PASS;
    }
    xil_printf("\r\n FAIL: too many errors (%d)\r\n", errs);
    return PERF_FAIL;
}

/* ---------------------------------------------------------------------------
 * Test C: Latency breakdown
 * --------------------------------------------------------------------------- */
typedef struct {
    u32 s1_rearm_start_us;
    u32 s2_tx_us;
    u32 s3_end_drain_us;
    u32 s4_rx_us;
    u32 s5_recv_us;
    u32 total_us;
} stage_sample_t;

static int encrypt_measure_stages(stage_sample_t *s)
{
    int status;
    u32 timeout;
    u32 t0, t1, t_total_start;
    u32 rx_len;

    g_dma_error = 0;

    t_total_start = timing_now();

    /* S1: rearm + start_pkt */
    t0 = timing_now();
    status = dma_rearm_rx();
    if (status != XST_SUCCESS) return 0;
    aes_start_packet();
    t1 = timing_now();
    s->s1_rearm_start_us = timing_cycles_to_us(t1 - t0);

    /* S2: TX submit + IOC wait */
    t0 = timing_now();
    status = dma_send_packet(perf_pkt_buf, 64);
    if (status != XST_SUCCESS) { aes_end_packet(); return 0; }
    timeout = 0;
    while (!g_dma_tx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 2000000) { aes_end_packet(); return 0; }
    }
    if (g_dma_error) { g_dma_error = 0; aes_end_packet(); return 0; }
    t1 = timing_now();
    s->s2_tx_us = timing_cycles_to_us(t1 - t0);

    /* S3: drain + end_pkt */
    t0 = timing_now();
    usleep(10);
    aes_end_packet();
    t1 = timing_now();
    s->s3_end_drain_us = timing_cycles_to_us(t1 - t0);

    /* S4: RX wait */
    t0 = timing_now();
    timeout = 0;
    while (!g_dma_rx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 500000) {
            if (dma_poll_rx(1500000) != XST_SUCCESS) return 0;
            break;
        }
    }
    if (g_dma_error) { g_dma_error = 0; return 0; }
    t1 = timing_now();
    s->s4_rx_us = timing_cycles_to_us(t1 - t0);

    /* S5: recv */
    t0 = timing_now();
    status = dma_recv_packet(perf_rx_buf, &rx_len);
    if (status != XST_SUCCESS) return 0;
    t1 = timing_now();
    s->s5_recv_us = timing_cycles_to_us(t1 - t0);

    s->total_us = timing_cycles_to_us(t1 - t_total_start);
    return 1;
}

static int test_latency_breakdown(const u8 *key, const u8 *iv)
{
    stage_sample_t samples[PERF_BREAKDOWN_N];
    u32 i;
    int errs = 0;

    xil_printf("\r\n[PERF C] Latency breakdown (%d packets, per-stage timing)\r\n",
               PERF_BREAKDOWN_N);

    if (aes_reset_and_reload(key, iv) != XST_SUCCESS) {
        xil_printf(" FAIL: reset failed\r\n");
        return PERF_FAIL;
    }

    init_test_packet(64);

    for (i = 0; i < PERF_BREAKDOWN_N; i++) {
        if (i > 0 && (i % PERF_RESET_EVERY) == 0) {
            aes_reset_and_reload(key, iv);
        }
        if (!encrypt_measure_stages(&samples[i])) {
            errs++;
            memset(&samples[i], 0, sizeof(samples[i]));
        }
    }

    u64 sum_s1 = 0, sum_s2 = 0, sum_s3 = 0, sum_s4 = 0, sum_s5 = 0, sum_tot = 0;
    u32 min_s1 = 0xFFFFFFFFu, min_s2 = 0xFFFFFFFFu, min_s3 = 0xFFFFFFFFu;
    u32 min_s4 = 0xFFFFFFFFu, min_s5 = 0xFFFFFFFFu;
    u32 max_s1 = 0, max_s2 = 0, max_s3 = 0, max_s4 = 0, max_s5 = 0;
    u32 valid = 0;

    for (i = 0; i < PERF_BREAKDOWN_N; i++) {
        if (samples[i].total_us == 0) continue;
        stage_sample_t *s = &samples[i];
        sum_s1 += s->s1_rearm_start_us; sum_s2 += s->s2_tx_us;
        sum_s3 += s->s3_end_drain_us;   sum_s4 += s->s4_rx_us;
        sum_s5 += s->s5_recv_us;        sum_tot += s->total_us;

        if (s->s1_rearm_start_us < min_s1) min_s1 = s->s1_rearm_start_us;
        if (s->s2_tx_us          < min_s2) min_s2 = s->s2_tx_us;
        if (s->s3_end_drain_us   < min_s3) min_s3 = s->s3_end_drain_us;
        if (s->s4_rx_us          < min_s4) min_s4 = s->s4_rx_us;
        if (s->s5_recv_us        < min_s5) min_s5 = s->s5_recv_us;

        if (s->s1_rearm_start_us > max_s1) max_s1 = s->s1_rearm_start_us;
        if (s->s2_tx_us          > max_s2) max_s2 = s->s2_tx_us;
        if (s->s3_end_drain_us   > max_s3) max_s3 = s->s3_end_drain_us;
        if (s->s4_rx_us          > max_s4) max_s4 = s->s4_rx_us;
        if (s->s5_recv_us        > max_s5) max_s5 = s->s5_recv_us;
        valid++;
    }

    if (valid == 0) {
        xil_printf(" FAIL: no valid samples\r\n");
        return PERF_FAIL;
    }

    u32 avg_s1 = (u32)(sum_s1 / valid);
    u32 avg_s2 = (u32)(sum_s2 / valid);
    u32 avg_s3 = (u32)(sum_s3 / valid);
    u32 avg_s4 = (u32)(sum_s4 / valid);
    u32 avg_s5 = (u32)(sum_s5 / valid);
    u32 avg_tot = (u32)(sum_tot / valid);

    u32 pct_s1 = (avg_tot ? (avg_s1 * 100) / avg_tot : 0);
    u32 pct_s2 = (avg_tot ? (avg_s2 * 100) / avg_tot : 0);
    u32 pct_s3 = (avg_tot ? (avg_s3 * 100) / avg_tot : 0);
    u32 pct_s4 = (avg_tot ? (avg_s4 * 100) / avg_tot : 0);
    u32 pct_s5 = (avg_tot ? (avg_s5 * 100) / avg_tot : 0);

    /* Persist to summary */
    g_results.brkdn_s1_us = avg_s1;
    g_results.brkdn_s2_us = avg_s2;
    g_results.brkdn_s3_us = avg_s3;
    g_results.brkdn_s4_us = avg_s4;
    g_results.brkdn_s5_us = avg_s5;
    g_results.brkdn_total_us = avg_tot;

    u32 pcts[5] = { pct_s1, pct_s2, pct_s3, pct_s4, pct_s5 };
    u32 max_pct = 0, max_idx = 0;
    for (i = 0; i < 5; i++) {
        if (pcts[i] > max_pct) { max_pct = pcts[i]; max_idx = i; }
    }
    g_results.brkdn_dominant_idx = max_idx;
    g_results.brkdn_dominant_pct = max_pct;

    xil_printf(" Valid samples : %lu / %d  (errors: %d)\r\n",
               (unsigned long)valid, PERF_BREAKDOWN_N, errs);
    xil_printf("-----------------------------------------------------------\r\n");
    xil_printf(" Stage                    | Avg(us) | Min(us) | Max(us) | %%  \r\n");
    xil_printf("-----------------------------------------------------------\r\n");
    xil_printf(" S1 rearm_rx + start_pkt  | %7lu | %7lu | %7lu | %3lu\r\n",
               (unsigned long)avg_s1, (unsigned long)min_s1,
               (unsigned long)max_s1, (unsigned long)pct_s1);
    xil_printf(" S2 TX submit + IOC wait  | %7lu | %7lu | %7lu | %3lu\r\n",
               (unsigned long)avg_s2, (unsigned long)min_s2,
               (unsigned long)max_s2, (unsigned long)pct_s2);
    xil_printf(" S3 drain + end_pkt (EOP) | %7lu | %7lu | %7lu | %3lu\r\n",
               (unsigned long)avg_s3, (unsigned long)min_s3,
               (unsigned long)max_s3, (unsigned long)pct_s3);
    xil_printf(" S4 RX IOC / poll wait    | %7lu | %7lu | %7lu | %3lu\r\n",
               (unsigned long)avg_s4, (unsigned long)min_s4,
               (unsigned long)max_s4, (unsigned long)pct_s4);
    xil_printf(" S5 recv_packet (memcpy)  | %7lu | %7lu | %7lu | %3lu\r\n",
               (unsigned long)avg_s5, (unsigned long)min_s5,
               (unsigned long)max_s5, (unsigned long)pct_s5);
    xil_printf("-----------------------------------------------------------\r\n");
    xil_printf(" TOTAL                    | %7lu us\r\n", (unsigned long)avg_tot);
    xil_printf("-----------------------------------------------------------\r\n");
    xil_printf(" Dominant stage: %s at %lu%% of total latency\r\n",
               stage_names_short[max_idx], (unsigned long)max_pct);

    if (errs < PERF_BREAKDOWN_N / 20) {
        xil_printf(" PASS: breakdown collected\r\n");
        return PERF_PASS;
    }
    xil_printf(" FAIL: too many errors\r\n");
    return PERF_FAIL;
}

/* ---------------------------------------------------------------------------
 * Test D: Throughput jitter
 * --------------------------------------------------------------------------- */
static int test_throughput_jitter(const u8 *key, const u8 *iv)
{
    u32 batch_times_us[PERF_JITTER_BATCHES];
    u32 i, j;
    int errs = 0;

    xil_printf("\r\n[PERF D] Throughput jitter "
               "(%d batches x %d packets, 64 bytes)\r\n",
               PERF_JITTER_BATCHES, PERF_JITTER_BATCH_N);

    init_test_packet(64);

    for (i = 0; i < PERF_JITTER_BATCHES; i++) {
        u32 t_start, t_end;

        if (aes_reset_and_reload(key, iv) != XST_SUCCESS) {
            xil_printf(" WARN: batch %lu reset failed\r\n", (unsigned long)i);
            batch_times_us[i] = 0;
            errs++;
            continue;
        }

        t_start = timing_now();
        for (j = 0; j < PERF_JITTER_BATCH_N; j++) {
            u32 rx_len = 0;
            if (!encrypt_one_packet(perf_pkt_buf, 64, perf_rx_buf, &rx_len)) {
                errs++;
            }
        }
        t_end = timing_now();

        batch_times_us[i] = timing_cycles_to_us(t_end - t_start);
    }

    u32 min_us = 0xFFFFFFFFu;
    u32 max_us = 0;
    u64 sum_us = 0;
    u32 valid = 0;
    for (i = 0; i < PERF_JITTER_BATCHES; i++) {
        if (batch_times_us[i] == 0) continue;
        if (batch_times_us[i] < min_us) min_us = batch_times_us[i];
        if (batch_times_us[i] > max_us) max_us = batch_times_us[i];
        sum_us += batch_times_us[i];
        valid++;
    }

    if (valid == 0) {
        xil_printf(" FAIL: no valid batches\r\n");
        return PERF_FAIL;
    }

    u32 mean_us = (u32)(sum_us / valid);
    u64 var_sum = 0;
    for (i = 0; i < PERF_JITTER_BATCHES; i++) {
        if (batch_times_us[i] == 0) continue;
        s32 d = (s32)batch_times_us[i] - (s32)mean_us;
        var_sum += (u64)(d * d);
    }
    u32 variance = (u32)(var_sum / valid);
    u32 stdev = 0;
    while ((stdev + 1) * (stdev + 1) <= variance) stdev++;

    /* CoV in parts per million for precision */
    u32 cov_ppm = (mean_us ? (u32)(((u64)stdev * 1000000) / mean_us) : 0);

    /* Persist */
    g_results.jitter_min_us   = min_us;
    g_results.jitter_max_us   = max_us;
    g_results.jitter_mean_us  = mean_us;
    g_results.jitter_stdev_us = stdev;
    g_results.jitter_cov_ppm  = cov_ppm;

    u64 bits_per_batch = (u64)PERF_JITTER_BATCH_N * 64 * 8;

    xil_printf("-----------------------------------------------\r\n");
    xil_printf(" Batch | Time(us)  | Throughput(Mbps)\r\n");
    xil_printf("-----------------------------------------------\r\n");
    for (i = 0; i < PERF_JITTER_BATCHES; i++) {
        if (batch_times_us[i] == 0) {
            xil_printf(" %5lu | SKIPPED\r\n", (unsigned long)i);
            continue;
        }
        u32 mbps_x100 = (u32)((bits_per_batch * 100) /
                              (batch_times_us[i] ? batch_times_us[i] : 1));
        xil_printf(" %5lu | %9lu | %3lu.%02lu\r\n",
                   (unsigned long)i,
                   (unsigned long)batch_times_us[i],
                   (unsigned long)(mbps_x100 / 100),
                   (unsigned long)(mbps_x100 % 100));
    }
    xil_printf("-----------------------------------------------\r\n");
    xil_printf(" Min batch time : %lu us\r\n", (unsigned long)min_us);
    xil_printf(" Max batch time : %lu us\r\n", (unsigned long)max_us);
    xil_printf(" Mean batch time: %lu us\r\n", (unsigned long)mean_us);
    xil_printf(" Stdev          : %lu us  (jitter)\r\n",
               (unsigned long)stdev);
    u32 cov_pct_x1000 = cov_ppm / 10;
    xil_printf(" CoV            : %lu.%03lu %%  (stdev/mean)\r\n",
               (unsigned long)(cov_pct_x1000 / 1000),
               (unsigned long)(cov_pct_x1000 % 1000));

    if (errs < (PERF_JITTER_BATCHES * PERF_JITTER_BATCH_N) / 20) {
        xil_printf(" PASS: jitter measurement complete\r\n");
        return PERF_PASS;
    }
    xil_printf(" FAIL: too many errors (%d)\r\n", errs);
    return PERF_FAIL;
}

/* ---------------------------------------------------------------------------
 * print_summary_table - copy/paste ready metrics for the report
 * --------------------------------------------------------------------------- */
static void print_summary_table(void)
{
    u32 peak_mbps = g_results.thrpt_peak_mbps_x100;
    u32 cov_pct_x1000 = g_results.jitter_cov_ppm / 10;
    u32 active_us = g_results.brkdn_s1_us + g_results.brkdn_s2_us +
                    g_results.brkdn_s3_us + g_results.brkdn_s5_us;

    xil_printf("\r\n========================================\r\n");
    xil_printf("  KEY METRICS SUMMARY (report-ready)\r\n");
    xil_printf("========================================\r\n");

    xil_printf("  --- Throughput ---\r\n");
    xil_printf("    Peak throughput (%lu B pkt)  : %lu.%02lu Mbps\r\n",
               (unsigned long)g_results.thrpt_peak_size,
               (unsigned long)(peak_mbps / 100),
               (unsigned long)(peak_mbps % 100));
    xil_printf("    Throughput @ 64  B pkt      : %lu.%02lu Mbps\r\n",
               (unsigned long)(g_results.thrpt_mbps_x100[0] / 100),
               (unsigned long)(g_results.thrpt_mbps_x100[0] % 100));
    xil_printf("    Throughput @ 1500 B pkt     : %lu.%02lu Mbps\r\n",
               (unsigned long)(g_results.thrpt_mbps_x100[5] / 100),
               (unsigned long)(g_results.thrpt_mbps_x100[5] % 100));
    xil_printf("    Throughput CoV (jitter)     : %lu.%03lu %%\r\n",
               (unsigned long)(cov_pct_x1000 / 1000),
               (unsigned long)(cov_pct_x1000 % 1000));

    xil_printf("  --- Latency (64 B packets) ---\r\n");
    xil_printf("    Mean                        : %lu us\r\n",
               (unsigned long)g_results.lat_mean_us);
    xil_printf("    Min / Max                   : %lu / %lu us\r\n",
               (unsigned long)g_results.lat_min_us,
               (unsigned long)g_results.lat_max_us);
    xil_printf("    Spread (max - min)          : %lu us\r\n",
               (unsigned long)g_results.lat_spread_us);
    xil_printf("    Stdev                       : %lu us\r\n",
               (unsigned long)g_results.lat_stdev_us);

    xil_printf("  --- Latency breakdown ---\r\n");
    xil_printf("    Active computation (S1+2+3+5) : %lu us\r\n",
               (unsigned long)active_us);
    xil_printf("    RX-wait stage (S4)            : %lu us\r\n",
               (unsigned long)g_results.brkdn_s4_us);
    xil_printf("    Dominant stage                : %s (%lu%%)\r\n",
               stage_names_short[g_results.brkdn_dominant_idx],
               (unsigned long)g_results.brkdn_dominant_pct);

    xil_printf("  --- Test configuration ---\r\n");
    xil_printf("    Reset interval              : every %d packets\r\n",
               PERF_RESET_EVERY);
    xil_printf("    Packet sizes tested         : 64/128/256/512/1024/1500 B\r\n");
    xil_printf("    Latency samples             : %d\r\n", PERF_LATENCY_N);
    xil_printf("    Breakdown samples           : %d\r\n", PERF_BREAKDOWN_N);
    xil_printf("    Jitter batches              : %d x %d packets\r\n",
               PERF_JITTER_BATCHES, PERF_JITTER_BATCH_N);

    xil_printf("========================================\r\n");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------- */
int run_performance_tests(const u8 *key, const u8 *iv)
{
    int results[4];
    int passed = 0;
    int i;

    memset(&g_results, 0, sizeof(g_results));

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf("  Performance Evaluation\r\n");
    xil_printf("========================================\r\n");

    timing_init();

    results[0] = test_throughput_sweep(key, iv);
    results[1] = test_latency_histogram(key, iv);
    results[2] = test_latency_breakdown(key, iv);
    results[3] = test_throughput_jitter(key, iv);

    xil_printf("\r\n========================================\r\n");
    xil_printf("  Performance Test Summary\r\n");
    xil_printf("========================================\r\n");
    xil_printf("  [%s] A: Throughput sweep\r\n",
               results[0] == PERF_PASS ? "PASS" : "FAIL");
    xil_printf("  [%s] B: Latency histogram\r\n",
               results[1] == PERF_PASS ? "PASS" : "FAIL");
    xil_printf("  [%s] C: Latency breakdown\r\n",
               results[2] == PERF_PASS ? "PASS" : "FAIL");
    xil_printf("  [%s] D: Throughput jitter\r\n",
               results[3] == PERF_PASS ? "PASS" : "FAIL");

    for (i = 0; i < 4; i++) {
        if (results[i] == PERF_PASS) passed++;
    }
    xil_printf("  Result: %d/4 automated tests passed\r\n", passed);
    xil_printf("========================================\r\n");

    /* Final copy/paste-ready summary */
    print_summary_table();

    return (passed == 4) ? PERF_PASS : PERF_FAIL;
}
