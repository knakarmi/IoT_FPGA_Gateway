/* =============================================================================
 * IoT Gateway - Week 7
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

static const u8 TEST_IV[12] = {
    0xF0, 0x76, 0x1E, 0x8D, 0xCD, 0x3D,
    0x00, 0x01, 0x76, 0xD4, 0x57, 0xED
};

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
    xil_printf("  ELE598 Research Project - Week 7\r\n");
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
    status = aes_load_key(TEST_KEY, TEST_IV);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: AES key load failed (%d)\r\n", status);
        return XST_FAILURE;
    }
    xil_printf("    OK - Key and IV loaded\r\n");

    /* ------------------------------------------------------------------
<<<<<<< HEAD
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
=======
>>>>>>> 3c2b6896d3e5cba170d24fac102d36292189d63c
     * 4. Run NIST test vector verification
     * ------------------------------------------------------------------ */
    xil_printf("[4] Running packet transfer test...\r\n");
    status = run_packet_test(TEST_PACKET, sizeof(TEST_PACKET));
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: Packet test failed (%d)\r\n", status);
        return XST_FAILURE;
    }
    xil_printf("    OK - Packet test passed\r\n");

<<<<<<< HEAD
    // Temp Changes
    static const u8 TEST_PACKET2[64] = {0}; /* all zeros */
    static const u8 TEST_PACKET3[64] = {0x1}; /* all 1 */
    xil_printf("[4a] Running Temporary packet transfer test... TEST_PACKET2[64] = {0}...\r\n");
    status = run_packet_test(TEST_PACKET2, sizeof(TEST_PACKET2));
    xil_printf("[4b] Running Temporary packet transfer test again... TEST_PACKET2[64] = {0}...\r\n");
    status = run_packet_test(TEST_PACKET2, sizeof(TEST_PACKET2));

=======
>>>>>>> 3c2b6896d3e5cba170d24fac102d36292189d63c
    /* ------------------------------------------------------------------
     * 5. Main processing loop
     * ------------------------------------------------------------------ */
    xil_printf("\r\n[5] Entering main processing loop...\r\n");
    xil_printf("    Waiting for DMA transfers and parser IRQs\r\n");
    xil_printf("========================================\r\n");

<<<<<<< HEAD
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
=======
    while (1) {
        /* Processing is interrupt-driven.
         * Main loop just prints periodic status. */
        usleep(1000000);  /* 1 second */
        print_status();
>>>>>>> 3c2b6896d3e5cba170d24fac102d36292189d63c
    }

    return XST_SUCCESS;
}
