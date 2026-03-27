/* =============================================================================
 * IoT Gateway - Week 7
 * File   : packet_test.c
 * Purpose: Send a test packet through the DMA->AES pipeline and verify output
 * =============================================================================
 */

#include "packet_test.h"
#include "dma_handler.h"
#include "xil_printf.h"
#include "sleep.h"
#include <string.h>

/* Timeout waiting for DMA completion (in microseconds) */
#define DMA_TIMEOUT_US      500000  /* 500 ms */

/* ---------------------------------------------------------------------------
 * run_packet_test
 * Send a known packet through MM2S -> AES -> S2MM and print the result
 * --------------------------------------------------------------------------- */
int run_packet_test(const u8 *pkt, u32 len)
{
    int status;
    u8  rx_buf[2048];
    u32 rx_len = 0;
    u32 timeout = 0;
    u32 i;

    xil_printf("PKT TEST: Sending %lu byte packet through AES pipeline\r\n",
               len);

    /* Print plaintext */
    xil_printf("PKT TEST: Plaintext (first 16 bytes):\r\n  ");
    for (i = 0; i < 16 && i < len; i++) {
        xil_printf("%02X ", pkt[i]);
    }
    xil_printf("\r\n");

    /* Send packet via DMA MM2S */
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
            xil_printf("PKT TEST: TX timeout!\r\n");
            return XST_FAILURE;
        }
    }

    if (g_dma_error) {
        xil_printf("PKT TEST: DMA error during TX\r\n");
        g_dma_error = 0;
        return XST_FAILURE;
    }

    xil_printf("PKT TEST: TX complete, waiting for RX...\r\n");

    /* Wait for RX completion (S2MM) */
    timeout = 0;
    while (!g_dma_rx_done && !g_dma_error) {
        usleep(100);
        timeout += 100;
        if (timeout >= DMA_TIMEOUT_US) {
            xil_printf("PKT TEST: RX timeout!\r\n");
            return XST_FAILURE;
        }
    }

    if (g_dma_error) {
        xil_printf("PKT TEST: DMA error during RX\r\n");
        g_dma_error = 0;
        return XST_FAILURE;
    }

    /* Read encrypted output */
    status = dma_recv_packet(rx_buf, &rx_len);
    if (status != XST_SUCCESS) {
        xil_printf("PKT TEST: dma_recv_packet failed\r\n");
        return status;
    }

    /* Print ciphertext */
    xil_printf("PKT TEST: Ciphertext received (%lu bytes):\r\n  ", rx_len);
    for (i = 0; i < rx_len && i < 32; i++) {
        xil_printf("%02X ", rx_buf[i]);
        if ((i+1) % 16 == 0) xil_printf("\r\n  ");
    }
    xil_printf("\r\n");

    /* Basic sanity check: output should not be identical to input */
    if (rx_len > 0 && memcmp(pkt, rx_buf, rx_len < len ? rx_len : len) == 0) {
        xil_printf("PKT TEST: WARNING - output identical to input!\r\n");
        xil_printf("          AES core may not be encrypting correctly\r\n");
        return XST_FAILURE;
    }

    xil_printf("PKT TEST: PASS - output differs from input (encryption active)\r\n");
    return XST_SUCCESS;
}
