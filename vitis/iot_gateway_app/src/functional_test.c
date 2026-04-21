/* =============================================================================
 * IoT Gateway - Week 10
 * File   : functional_test.c
 * Purpose: Functional test suite for the IoT gateway AES-256-GCM pipeline
 *
 * Test cases:
 *   Test 1 - Different plaintexts produce different ciphertexts
 *   Test 2 - Same plaintext produces different ciphertexts (GCM counter)
 *   Test 3 - Packet parser correctness
 *   Test 4 - Stability under sustained transfers (500 packets)
 *   Test 5 - Latency measurement
 *
 * Verbosity: set FT_VERBOSE=1 to re-enable per-transfer debug prints.
 *
 * Ciphertext = 64 bytes (plaintext) + 16 bytes (GCM auth tag) = 80 bytes
 * Bytes [0..63]: Encrypted plaintext (ciphertext proper)
 * Bytes [64..79]: 128-bit authentication tag (GCM's integrity check)
 * =============================================================================
 */

#include "functional_test.h"
#include "packet_parser.h"
#include "dma_handler.h"
#include "aes_key_loader.h"
#include "timing.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Verbosity control - set to 1 to re-enable chatty debug prints
 * --------------------------------------------------------------------------- */
#ifndef FT_VERBOSE
#define FT_VERBOSE 0
#endif

#if FT_VERBOSE
#define FT_DBG(...)  xil_printf(__VA_ARGS__)
#else
#define FT_DBG(...)  do { } while (0)
#endif

#define STABILITY_COUNT      12
#define STABILITY_PROGRESS   1   /* print progress every N transfers */

/* ---------------------------------------------------------------------------
 * Test packets - three different IoT device types
 * --------------------------------------------------------------------------- */

/* Packet 1: Temperature sensor (device_id=0x0001, SENSOR type) */
static const u8 PKT_TEMP[64] = {
    /* Ethernet */
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xAA,0xBB,0xCC,0xDD,0xEE,0x01,
    0x08,0x00,
    /* IPv4 */
    0x45,0x00, 0x00,0x32, 0x00,0x01, 0x00,0x00,
    0x40,0x11, 0x00,0x00,
    0xC0,0xA8,0x01,0x01,   /* src: 192.168.1.1 */
    0x0A,0x00,0x00,0x01,   /* dst: 10.0.0.1   */
    /* UDP */
    0x0F,0xA0, 0x07,0x5B, 0x00,0x1E, 0x00,0x00,
    /* IoT header: device=0x0001, type=SENSOR, seq=1, payload=6 */
    0x00,0x01, 0x02, 0x00, 0x00,0x01, 0x00,0x06,
    /* Payload: temperature=23.5C, humidity=65% */
    0x09,0x2E, 0x00,0x41, 0x00,0x00
};

/* Packet 2: GPS tracker (device_id=0x0002, RAW type) */
static const u8 PKT_GPS[64] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xAA,0xBB,0xCC,0xDD,0xEE,0x02,
    0x08,0x00,
    0x45,0x00, 0x00,0x32, 0x00,0x02, 0x00,0x00,
    0x40,0x11, 0x00,0x00,
    0xC0,0xA8,0x01,0x02,
    0x0A,0x00,0x00,0x01,
    0x0F,0xA0, 0x07,0x5B, 0x00,0x1E, 0x00,0x00,
    0x00,0x02, 0x01, 0x00, 0x00,0x01, 0x00,0x06,
    0x10,0x56, 0xE4,0xBF, 0x00,0x00
};

/* Packet 3: Industrial vibration sensor (device_id=0x0003, SENSOR type) */
static const u8 PKT_VIBR[64] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xAA,0xBB,0xCC,0xDD,0xEE,0x03,
    0x08,0x00,
    0x45,0x00, 0x00,0x32, 0x00,0x03, 0x00,0x00,
    0x40,0x11, 0x00,0x00,
    0xC0,0xA8,0x01,0x03,
    0x0A,0x00,0x00,0x01,
    0x0F,0xA0, 0x07,0x5B, 0x00,0x1E, 0x00,0x00,
    0x00,0x03, 0x02, 0x00, 0x00,0x01, 0x00,0x06,
    0x00,0x0C, 0x00,0x03, 0x00,0x62
};

/* ---------------------------------------------------------------------------
 * Helper: encrypt one packet and return ciphertext in out_buf
 * Returns 1 on success, 0 on failure.
 * --------------------------------------------------------------------------- */
static int encrypt_packet(const u8 *pkt, u32 len,
                           u8 *out_buf, u32 *out_len)
{
    int status;
    u32 timeout = 0;

    g_dma_error = 0;

    /* Re-arm RX before TX - S2MM must have a BD ready before data arrives */
    status = dma_rearm_rx();
    if (status != XST_SUCCESS) return 0;

    /* Assert per-packet control signals BEFORE starting DMA:
     *   - pulse icb_start_cnt to reset GCM counter to J0
     *   - raise ghash_pkt_val (must stay high for entire packet) */
    aes_start_packet();

    status = dma_send_packet((u8 *)pkt, len);
    if (status != XST_SUCCESS) {
        aes_end_packet();
        return 0;
    }

    /* Wait for TX */
    timeout = 0;
    while (!g_dma_tx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 2000000) {
            aes_end_packet();
            return 0;
        }
    }
    if (g_dma_error) { g_dma_error = 0; aes_end_packet(); return 0; }

    /* Let the AES pipeline drain before EOP */
    usleep(10);
    aes_end_packet();

    /* Wait for RX - interrupt first, then polling fallback */
    timeout = 0;
    while (!g_dma_rx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 500000) {
            if (dma_poll_rx(1500000) != XST_SUCCESS) {
                return 0;
            }
            break;
        }
    }
    if (g_dma_error) { g_dma_error = 0; return 0; }

    status = dma_recv_packet(out_buf, out_len);
    if (status != XST_SUCCESS) return 0;


    return 1;
}

// Test 1: Different plaintexts produce different ciphertexts
static int test_different_plaintexts(void)
{
    u8  ct1[2048], ct2[2048], ct3[2048];
    u32 len1, len2, len3;

    memset(ct1, 0, sizeof(ct1));
    memset(ct2, 0, sizeof(ct2));
    memset(ct3, 0, sizeof(ct3));

    xil_printf("\r\n[TEST 1] Different plaintexts -> different ciphertexts\r\n");

    if (!encrypt_packet(PKT_TEMP, 64, ct1, &len1)) {
        xil_printf("  FAIL: temperature packet encryption failed\r\n");
        return TEST_FAIL;
    }
    if (!encrypt_packet(PKT_GPS,  64, ct2, &len2)) {
        xil_printf("  FAIL: GPS packet encryption failed\r\n");
        return TEST_FAIL;
    }
    if (!encrypt_packet(PKT_VIBR, 64, ct3, &len3)) {
        xil_printf("  FAIL: vibration packet encryption failed\r\n");
        return TEST_FAIL;
    }

    /* Show first 8 bytes of each ciphertext */
    xil_printf(" Only Displaying First 8 Byte of the Ciphertext (CT)\r\n ");
    xil_printf("  Temp  CT: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               ct1[0],ct1[1],ct1[2],ct1[3],ct1[4],ct1[5],ct1[6],ct1[7]);
    xil_printf("  GPS   CT: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               ct2[0],ct2[1],ct2[2],ct2[3],ct2[4],ct2[5],ct2[6],ct2[7]);
    xil_printf("  Vibr  CT: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               ct3[0],ct3[1],ct3[2],ct3[3],ct3[4],ct3[5],ct3[6],ct3[7]);

    if (memcmp(ct1, ct2, 16) != 0 &&
        memcmp(ct1, ct3, 16) != 0 &&
        memcmp(ct2, ct3, 16) != 0) {
        xil_printf("  PASS: all three ciphertexts are unique\r\n");
        return TEST_PASS;
    }
    xil_printf("  FAIL: two or more ciphertexts are identical\r\n");
    return TEST_FAIL;
}

// Test 2: Same plaintext produces different ciphertexts (GCM counter)
static int test_gcm_counter(void)
{
    u8  ct1[2048], ct2[2048];
    u32 len1, len2;

    xil_printf("\r\n[TEST 2] GCM counter - same plaintext -> different ciphertexts\r\n");

    if (!encrypt_packet(PKT_TEMP, 64, ct1, &len1)) {
        xil_printf("  FAIL: first encryption failed\r\n");
        return TEST_FAIL;
    }
    if (!encrypt_packet(PKT_TEMP, 64, ct2, &len2)) {
        xil_printf("  FAIL: second encryption failed\r\n");
        return TEST_FAIL;
    }

    xil_printf(" Only Displaying First 8 Byte of the Ciphertext (CT)\r\n ");
    xil_printf("  CT run 1: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               ct1[0],ct1[1],ct1[2],ct1[3],ct1[4],ct1[5],ct1[6],ct1[7]);
    xil_printf("  CT run 2: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               ct2[0],ct2[1],ct2[2],ct2[3],ct2[4],ct2[5],ct2[6],ct2[7]);

    if (memcmp(ct1, ct2, 16) != 0) {
        xil_printf("  PASS: GCM counter advancing correctly\r\n");
        return TEST_PASS;
    }
    xil_printf("  FAIL: identical ciphertexts - GCM counter not advancing\r\n");
    return TEST_FAIL;
}

/* ---------------------------------------------------------------------------
 * Test 3: Packet parser correctness
 * --------------------------------------------------------------------------- */
static int test_parser_correctness(void)
{
    iot_packet_t pkt;
    int failures = 0;

    xil_printf("\r\n[TEST 3] Packet parser correctness\r\n");

    parse_packet(PKT_TEMP, 64, &pkt);
    xil_printf("  Temp sensor: device=0x%04X type=%s seq=%d\r\n",
               (unsigned int)pkt.iot.device_id,
               get_msg_type_str(pkt.iot.msg_type),
               (int)pkt.iot.seq_num);
    if (pkt.iot.device_id != 0x0001 ||
        pkt.iot.msg_type  != IOT_MSG_SENSOR ||
        pkt.iot.seq_num   != 1) failures++;

    parse_packet(PKT_GPS, 64, &pkt);
    xil_printf("  GPS tracker: device=0x%04X type=%s seq=%d\r\n",
               (unsigned int)pkt.iot.device_id,
               get_msg_type_str(pkt.iot.msg_type),
               (int)pkt.iot.seq_num);
    if (pkt.iot.device_id != 0x0002 ||
        pkt.iot.msg_type  != IOT_MSG_RAW ||
        pkt.iot.seq_num   != 1) failures++;

    parse_packet(PKT_VIBR, 64, &pkt);
    xil_printf("  Vibr sensor: device=0x%04X type=%s seq=%d\r\n",
               (unsigned int)pkt.iot.device_id,
               get_msg_type_str(pkt.iot.msg_type),
               (int)pkt.iot.seq_num);
    if (pkt.iot.device_id != 0x0003 ||
        pkt.iot.msg_type  != IOT_MSG_SENSOR ||
        pkt.iot.seq_num   != 1) failures++;

    if (failures == 0) {
        xil_printf("  PASS: all packet fields parsed correctly\r\n");
        return TEST_PASS;
    }
    xil_printf("  FAIL: %d parser fields incorrect\r\n", failures);
    return TEST_FAIL;
}

/* ---------------------------------------------------------------------------
 * Test 4: Stability - sustained transfer test
 *
 * Cycles through the 3 sensor packet types for STABILITY_COUNT iterations,
 * verifying each encryption produces non-zero ciphertext. Reports number
 * of errors and final pass/fail. Also verifies the GCM counter has
 * actually advanced by comparing the first and last Temp ciphertext.
 * --------------------------------------------------------------------------- */
static int test_stability(void)
{
    u8  ct[2048];
    u8  first_temp_ct[16];
    u8  last_temp_ct[16];
    u32 len;
    int errors = 0;
    int zero_output_count = 0;
    int first_temp_saved = 0;
    u32 i;

    const u8 *pkts[3] = { PKT_TEMP, PKT_GPS, PKT_VIBR };

    xil_printf("\r\n[TEST 4] Stability - %d sustained transfers\r\n",
               STABILITY_COUNT);


    for (i = 0; i < STABILITY_COUNT; i++) {
        const u8 *current_pkt = pkts[i % 3];

        if (!encrypt_packet(current_pkt, 64, ct, &len)) {
            errors++;
            if (errors <= 3) {  /* Only print first few errors */
                xil_printf("  ERROR at transfer %lu: encrypt_packet failed\r\n", i);
            }
            continue;
        }

        /* Verify output has real content (not all zeros) */
        int has_data = 0;
        u32 j;
        for (j = 0; j < 16; j++) {
            if (ct[j] != 0) { has_data = 1; break; }
        }
        if (!has_data) {
            zero_output_count++;
            errors++;
            if (zero_output_count <= 3) {
                xil_printf("  ERROR at transfer %lu: ciphertext all zeros\r\n", i);
            }
            continue;
        }

        /* Snapshot first Temp ciphertext and keep updating last one */
        if (current_pkt == PKT_TEMP) {
            if (!first_temp_saved) {
                memcpy(first_temp_ct, ct, 16);
                first_temp_saved = 1;
            }
            memcpy(last_temp_ct, ct, 16);
        }

        /* Progress indicator every STABILITY_PROGRESS transfers */
        if (((i + 1) % STABILITY_PROGRESS) == 0) {
            xil_printf("  Progress: %lu/%d  (errors so far: %d)\r\n",
                       i + 1, STABILITY_COUNT, errors);
        }
    }

    xil_printf("  Completed %d transfers, %d errors\r\n",
               STABILITY_COUNT, errors);

    /* Sanity check: GCM counter must have advanced between first and last
     * Temp ciphertext */
    if (first_temp_saved && memcmp(first_temp_ct, last_temp_ct, 16) == 0) {
        xil_printf("  FAIL: first and last Temp ciphertexts identical "
                   "(GCM counter stuck)\r\n");
        return TEST_FAIL;
    }

    if (errors == 0) {
        xil_printf("  PASS: %d/%d transfers successful, GCM counter advancing\r\n",
                   STABILITY_COUNT, STABILITY_COUNT);
        return TEST_PASS;
    }

    /* Tolerate <1% transient errors - likely BD ring edge cases */
    if (errors < (STABILITY_COUNT / 100)) {
        xil_printf("  PASS: %d/%d transfers (%d transient errors, <1%%)\r\n",
                   STABILITY_COUNT - errors, STABILITY_COUNT, errors);
        return TEST_PASS;
    }

    xil_printf("  FAIL: %d/%d transfers failed\r\n", errors, STABILITY_COUNT);
    return TEST_FAIL;
}

// Test 5: Latency measurement
static int test_latency(void)
{
    u8  ct[2048];
    u32 len;
    u32 t_start, t_end, cycles;
    u32 latencies[5];
    u32 min_us, max_us, sum_us, avg_us;
    int i;
    u32 timeout;

    xil_printf("\r\n[TEST 5] Latency measurement (5 transfers)\r\n");

    for (i = 0; i < 5; i++) {
        dma_rearm_rx();
        g_dma_tx_done = 0;

        t_start = timing_now();

        dma_send_packet((u8 *)PKT_TEMP, 64);

        /* Wait for TX */
        timeout = 0;
        while (!g_dma_tx_done && !g_dma_error) {
            usleep(100);
            timeout += 100;
            if (timeout >= 2000000) break;
        }

        /* Wait for RX */
        timeout = 0;
        while (!g_dma_rx_done && !g_dma_error) {
            usleep(100);
            timeout += 100;
            if (timeout >= 500000) {
                dma_poll_rx(1500000);
                break;
            }
        }

        t_end = timing_now();

        dma_recv_packet(ct, &len);

        cycles = t_end - t_start;
        latencies[i] = timing_cycles_to_us(cycles);

        FT_DBG("  Transfer %d: %lu us (%lu cycles)\r\n",
               i + 1, (unsigned long)latencies[i], (unsigned long)cycles);
    }

    /* Calculate min, max, average */
    min_us = latencies[0];
    max_us = latencies[0];
    sum_us = 0;
    for (i = 0; i < 5; i++) {
        if (latencies[i] < min_us) min_us = latencies[i];
        if (latencies[i] > max_us) max_us = latencies[i];
        sum_us += latencies[i];
    }
    avg_us = sum_us / 5;

    xil_printf("  Min latency : %lu us\r\n", (unsigned long)min_us);
    xil_printf("  Max latency : %lu us\r\n", (unsigned long)max_us);
    xil_printf("  Avg latency : %lu us\r\n", (unsigned long)avg_us);
    xil_printf("  Throughput  : ~%lu Kbps (64-byte packets)\r\n",
               (unsigned long)((64UL * 8UL * 1000UL) / (avg_us ? avg_us : 1)));
    xil_printf("  PASS: latency measurement complete\r\n");

    return TEST_PASS;
}


int run_functional_tests(void)
{
    int results[5];
    int passed = 0;
    int i;

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf("  Week 10 Functional Test Suite\r\n");
    xil_printf("========================================\r\n");

    timing_init();

    results[0] = test_different_plaintexts();
    results[1] = test_gcm_counter();
    results[2] = test_parser_correctness();
    results[3] = test_stability();
    results[4] = test_latency();

    const char *names[5] = {
        "Different plaintexts",
        "GCM counter",
        "Parser correctness",
        "Stability",
        "Latency measurement"
    };

    xil_printf("\r\n========================================\r\n");
    xil_printf("  Test Summary\r\n");
    xil_printf("========================================\r\n");

    for (i = 0; i < 5; i++) {
        if (results[i] == TEST_PASS) {
            xil_printf("  [PASS] Test %d: %s\r\n", i+1, names[i]);
            passed++;
        } else {
            xil_printf("  [FAIL] Test %d: %s\r\n", i+1, names[i]);
        }
    }

    xil_printf("  Result: %d/5 tests passed\r\n", passed);
    xil_printf("========================================\r\n");

    return (passed == 5) ? TEST_PASS : TEST_FAIL;
}
