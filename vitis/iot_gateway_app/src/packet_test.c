/* =============================================================================
 * IoT Gateway - Week 10
 * File   : packet_test.c
 * Purpose: Send a test packet through the DMA->AES pipeline and verify output
 *
 * UART output is minimal in normal operation - set PKT_VERBOSE to 1
 * to re-enable per-packet plaintext/ciphertext hex dumps.
 * =============================================================================
 */

#include "packet_test.h"
#include "dma_handler.h"
#include "interrupt_handler.h"
#include "xil_printf.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "sleep.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Verbosity control: set to 1 to re-enable chatty debug prints
 * --------------------------------------------------------------------------- */
#ifndef PKT_VERBOSE
#define PKT_VERBOSE 0
#endif

#if PKT_VERBOSE
#define PKT_DBG(...)  xil_printf(__VA_ARGS__)
#else
#define PKT_DBG(...)  do { } while (0)
#endif

/* DMA register offsets for manual status read on timeout */
#define DMA_MM2S_SR_OFFSET   0x04
#define DMA_S2MM_SR_OFFSET   0x34
#define DMA_MM2S_CR_OFFSET   0x00
#define DMA_S2MM_CR_OFFSET   0x30

/* Timeout: 2 s - AES key expansion + first block can be slow on some cores */
#define DMA_TIMEOUT_US      2000000

/* ---------------------------------------------------------------------------
 * dump_dma_status - only called on errors / timeouts
 * --------------------------------------------------------------------------- */
static void dump_dma_status(void)
{
    u32 base = XPAR_AXI_DMA_0_BASEADDR;
    u32 mm2s_cr = Xil_In32(base + DMA_MM2S_CR_OFFSET);
    u32 mm2s_sr = Xil_In32(base + DMA_MM2S_SR_OFFSET);
    u32 s2mm_cr = Xil_In32(base + DMA_S2MM_CR_OFFSET);
    u32 s2mm_sr = Xil_In32(base + DMA_S2MM_SR_OFFSET);

    xil_printf("DMA REGS:\r\n");
    xil_printf("  MM2S CR=0x%08X  SR=0x%08X\r\n",
               (unsigned int)mm2s_cr, (unsigned int)mm2s_sr);
    xil_printf("  S2MM CR=0x%08X  SR=0x%08X\r\n",
               (unsigned int)s2mm_cr, (unsigned int)s2mm_sr);
}

/* ---------------------------------------------------------------------------
 * run_packet_test
 * Send a known packet through MM2S -> AES -> S2MM and verify the result.
 * --------------------------------------------------------------------------- */
int run_packet_test(const u8 *pkt, u32 len)
{
    int status;
    u8  rx_buf[2048];
    u32 rx_len = 0;
    u32 timeout = 0;
    u32 i;

    PKT_DBG("PKT TEST: Sending %lu byte packet through AES pipeline\r\n", len);

#if PKT_VERBOSE
    xil_printf("PKT TEST: Plaintext (first 16 bytes):\r\n  ");
    for (i = 0; i < 16 && i < len; i++) {
        xil_printf("%02X ", pkt[i]);
    }
    xil_printf("\r\n");
#endif

    /* Re-arm RX BD ring BEFORE sending TX - S2MM must have a buffer
     * descriptor ready before data arrives, or it stalls silently. */
    status = dma_rearm_rx();
    if (status != XST_SUCCESS) {
        xil_printf("PKT TEST: dma_rearm_rx failed\r\n");
        return status;
    }

    status = dma_send_packet((u8 *)pkt, len);
    if (status != XST_SUCCESS) {
        xil_printf("PKT TEST: dma_send_packet failed\r\n");
        return status;
    }

    /* Wait for TX completion */
    timeout = 0;
    while (!g_dma_tx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= DMA_TIMEOUT_US) {
            xil_printf("PKT TEST: TX timeout\r\n");
            dump_dma_status();
            return XST_FAILURE;
        }
    }

    if (g_dma_error) {
        xil_printf("PKT TEST: DMA error during TX\r\n");
        dump_dma_status();
        g_dma_error = 0;
        return XST_FAILURE;
    }

    /* Wait for RX via interrupt (500 ms), then fall back to polling. */
    timeout = 0;
    while (!g_dma_rx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= 500000) {
            /* Poll S2MM SR directly - bypasses GIC entirely */
            if (dma_poll_rx(1500000) != XST_SUCCESS) {
                xil_printf("PKT TEST: RX timeout\r\n");
                dump_dma_status();
                return XST_FAILURE;
            }
            break;
        }
    }

    if (g_dma_error) {
        xil_printf("PKT TEST: DMA error during RX\r\n");
        dump_dma_status();
        g_dma_error = 0;
        return XST_FAILURE;
    }

    /* Read encrypted output */
    status = dma_recv_packet(rx_buf, &rx_len);
    if (status != XST_SUCCESS) {
        xil_printf("PKT TEST: dma_recv_packet failed\r\n");
        return status;
    }

    if (rx_len == 0) {
        xil_printf("PKT TEST: FAIL - no data returned from AES\r\n");
        return XST_FAILURE;
    }

#if PKT_VERBOSE
    xil_printf("PKT TEST: Ciphertext (%lu bytes):\r\n  ", rx_len);
    for (i = 0; i < rx_len && i < 32; i++) {
        xil_printf("%02X ", rx_buf[i]);
        if ((i+1) % 16 == 0) xil_printf("\r\n  ");
    }
    xil_printf("\r\n");
#endif

    /* Sanity check: encrypted output must differ from plaintext */
    if (memcmp(pkt, rx_buf, rx_len < len ? rx_len : len) == 0) {
        xil_printf("PKT TEST: FAIL - output identical to input (no encryption)\r\n");
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}
