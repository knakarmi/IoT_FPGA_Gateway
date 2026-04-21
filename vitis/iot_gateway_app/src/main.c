/* =============================================================================
 * IoT Gateway - Week 10
 * File   : main.c
 * Target : Zynq ZC706 PS (ARM Cortex-A9)
 * Purpose: Main entry point - initializes all subsystems and runs the
 *          packet processing loop
 *
 * Subsystems initialized:
 *   1. Interrupt controller (GIC)
 *   2. AXI DMA (scatter-gather mode)
 *   3. AES key/IV loader
 *   4. Packet processing loop
 * =============================================================================
 */

#include "xparameters.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"

#include "dma_handler.h"
#include "aes_key_loader.h"
#include "interrupt_handler.h"
#include "packet_test.h"
#include "packet_parser.h"
#include "functional_test.h"
#include "timing.h"

/* ---------------------------------------------------------------------------
 * NIST AES-256-GCM Test Vector (SP 800-38D)
 * Key  : 256-bit
 * IV   : 96-bit nonce
 * Plain: known test pattern
 * --------------------------------------------------------------------------- */
static const u8 TEST_KEY[32] = {
    0x69, 0x1D, 0x3E, 0xE9, 0x09, 0xD7, 0xF5, 0x41,
    0x67, 0xFD, 0x1C, 0xA0, 0xB5, 0xD7, 0x69, 0x08,
    0x1F, 0x2B, 0xDE, 0x1A, 0xEE, 0x65, 0x5F, 0xDB,
    0xAB, 0x80, 0xBD, 0x52, 0x95, 0xAE, 0x6B, 0xE7
};

/* IV is generated at runtime using the ARM cycle counter so each
 * program run produces unique ciphertext. In production this would
 * be a cryptographically random nonce managed by a key server. */
static u8 TEST_IV[12];

/* ---------------------------------------------------------------------------
 * Test packet: minimal IPv4/UDP/IoT frame (64 bytes)
 * Dest MAC: FF:FF:FF:FF:FF:FF (broadcast)
 * Src  MAC: AA:BB:CC:DD:EE:FF
 * EtherType: 0x0800 (IPv4)
 * IP src: 192.168.1.10, dst: 10.0.0.1
 * UDP dst port: 1883 (MQTT)
 * IoT msg_type: 0x01 (RAW)
 * --------------------------------------------------------------------------- */
static const u8 TEST_PACKET[64] = {
    /* Ethernet header (14 bytes) */
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,           /* dst MAC */
    0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,           /* src MAC */
    0x08,0x00,                                /* EtherType: IPv4 */
    /* IPv4 header (20 bytes) */
    0x45,0x00,                                /* ver=4, IHL=5, DSCP=0 */
    0x00,0x32,                                /* total length = 50 */
    0x12,0x34,                                /* ID */
    0x00,0x00,                                /* flags/offset = 0 (no frag) */
    0x40,0x11,                                /* TTL=64, proto=UDP */
    0x00,0x00,                                /* checksum (ignored) */
    0xC0,0xA8,0x01,0x0A,                      /* src IP: 192.168.1.10 */
    0x0A,0x00,0x00,0x01,                      /* dst IP: 10.0.0.1 */
    /* UDP header (8 bytes) */
    0x0F,0xA0,                                /* src port: 4000 */
    0x07,0x5B,                                /* dst port: 1883 (MQTT) */
    0x00,0x1E,                                /* length: 30 */
    0x00,0x00,                                /* checksum */
    /* IoT app header (8 bytes) */
    0xBE,0xEF,                                /* device_id: 0xBEEF */
    0x01,                                     /* msg_type: RAW */
    0x00,                                     /* flags */
    0x00,0x01,                                /* seq_num: 1 */
    0x00,0x06,                                /* payload_len: 6 */
    /* Payload (6 bytes) */
    0xDE,0xAD,0xBE,0xEF,0xCA,0xFE
};

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(void)
{
    int status;

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf("  IoT Gateway - Zynq ZC706\r\n");
    xil_printf("  ELE598 Research Project - Week 10\r\n");
    xil_printf("========================================\r\n");

    /* Disable caches for DMA regions (important for coherency) */
    Xil_DCacheDisable();

    /* ------------------------------------------------------------------
     * 1. Initialize interrupt controller
     * ------------------------------------------------------------------ */
    xil_printf("[1] Initializing interrupt controller...\r\n");
    status = interrupt_init();
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: Interrupt init failed (%d)\r\n", status);
        return XST_FAILURE;
    }
    xil_printf("    OK\r\n");

    /* ------------------------------------------------------------------
     * 2. Initialize AXI DMA
     * ------------------------------------------------------------------ */
    xil_printf("[2] Initializing AXI DMA...\r\n");
    status = dma_init();
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: DMA init failed (%d)\r\n", status);
        return XST_FAILURE;
    }
    xil_printf("    OK - DMA ready\r\n");

    /* ------------------------------------------------------------------
     * 3. Load AES-256 key and IV into PL
     * ------------------------------------------------------------------ */
    xil_printf("[3] Loading AES-256 key and IV...\r\n");
    /* Generate unique IV from cycle counter for this session */
    {
    	/* Use XADC temp to vary sleep duration, then read cycle counter */
    	u32 xadc_temp = Xil_In32(0xF8007200) & 0xFFF;
    	usleep(xadc_temp);  /* sleep 0-4095 us based on temperature */
    	u32 t0 = timing_now();
    	u32 t1 = t0 ^ (xadc_temp << 20) ^ (xadc_temp * 1234567);
    	u32 t2 = t1 ^ (t0 >> 3) ^ 0xDEADBEEF;

    	TEST_IV[0]  = (t0 >> 24)&0xFF; TEST_IV[1]  = (t0 >> 16)&0xFF;
    	TEST_IV[2]  = (t0 >>  8)&0xFF; TEST_IV[3]  = (t0      )&0xFF;
    	TEST_IV[4]  = (t1 >> 24)&0xFF; TEST_IV[5]  = (t1 >> 16)&0xFF;
    	TEST_IV[6]  = (t1 >>  8)&0xFF; TEST_IV[7]  = (t1      )&0xFF;
    	TEST_IV[8]  = (t2 >> 24)&0xFF; TEST_IV[9]  = (t2 >> 16)&0xFF;
    	TEST_IV[10] = (t2 >>  8)&0xFF; TEST_IV[11] = (t2      )&0xFF;

        xil_printf("    IV: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                   TEST_IV[0],TEST_IV[1],TEST_IV[2],TEST_IV[3],
                   TEST_IV[4],TEST_IV[5],TEST_IV[6],TEST_IV[7],
                   TEST_IV[8],TEST_IV[9],TEST_IV[10],TEST_IV[11]);
    }
    status = aes_load_key(TEST_KEY, TEST_IV);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: AES key load failed (%d)\r\n", status);
        return XST_FAILURE;
    }
    xil_printf("    OK - Key and IV loaded\r\n");

    /* ------------------------------------------------------------------
     * 3b. Wait for AES core to assert ready after key load
     *     The core needs several clock cycles to expand the key schedule.
     *     Poll ready_o via STATUS GPIO with a timeout.
     * ------------------------------------------------------------------ */
    xil_printf("[3b] Waiting for AES core ready...\r\n");
    {
        int aes_ready = 0;
        u32 aes_poll;
        for (aes_poll = 0; aes_poll < 200; aes_poll++) {
            if (aes_verify_ready() == XST_SUCCESS) {
                aes_ready = 1;
                break;
            }
            usleep(1000);   /* 1 ms per poll, 200 ms max */
        }
        if (!aes_ready) {
            xil_printf("WARNING: AES ready_o never asserted after %lu ms\r\n",
                       (u32)(aes_poll));
            xil_printf("         Proceeding anyway - check GPIO STATUS wiring\r\n");
        } else {
            xil_printf("    OK - AES ready after ~%lu ms\r\n",
                       (u32)(aes_poll));
        }
    }

    /* ------------------------------------------------------------------
     * 3c. Parse the test packet to verify header fields
     * ------------------------------------------------------------------ */
    xil_printf("[3c] Parsing test packet headers...\r\n");
    {
        iot_packet_t parsed_pkt;
        int parse_result = parse_packet(TEST_PACKET, sizeof(TEST_PACKET),
                                        &parsed_pkt);
        if (parse_result == PARSE_OK) {
            print_packet_info(&parsed_pkt);
            xil_printf("    OK - Packet parsed successfully\r\n");
        } else {
            xil_printf("    WARNING: Parse failed (code %d)\r\n",
                       parse_result);
        }
    }

    /* ------------------------------------------------------------------
     * 4. Run Week 10 functional test suite
     * ------------------------------------------------------------------ */
    xil_printf("[4] Running functional test suite...\r\n");
    run_functional_tests();
    xil_printf("    OK - Functional tests complete\r\n");

    /* ------------------------------------------------------------------
     * 5. Main processing loop
     * ------------------------------------------------------------------ */
    xil_printf("\r\n[5] Entering main processing loop...\r\n");
    xil_printf("    Waiting for DMA transfers and parser IRQs\r\n");
    xil_printf("========================================\r\n");

    {
        /* Use a simple volatile counter for the status print delay.
         * usleep() on standalone BSP can hang if the private timer
         * is not initialized - this avoids that dependency entirely. */
        volatile u32 loop_count = 0;
        const u32 LOOPS_PER_SEC = 50000000U; /* ~1 s on Cortex-A9 @ 667 MHz */

        while (1) {
            loop_count++;
            if (loop_count >= LOOPS_PER_SEC) {
                loop_count = 0;
                print_status();
            }
        }
    }

    return XST_SUCCESS;
}
