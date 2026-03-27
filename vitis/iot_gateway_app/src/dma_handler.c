/* =============================================================================
 * IoT Gateway - Week 7
 * File   : dma_handler.c
 * Purpose: AXI DMA scatter-gather initialization and transfer management
 *
 * The AXI DMA is configured in scatter-gather mode:
 *   MM2S (Memory-to-Stream): reads plaintext from DDR3 → sends to AES core
 *   S2MM (Stream-to-Memory): receives ciphertext from AES core → writes DDR3
 *
 * Buffer descriptor rings are allocated in DDR3.
 * Interrupts fire when transfer completes (mm2s_introut, s2mm_introut).
 * =============================================================================
 */

#include "dma_handler.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_exception.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * DMA instance
 * --------------------------------------------------------------------------- */
static XAxiDma dma_inst;

/* ---------------------------------------------------------------------------
 * TX/RX buffers — aligned to cache line boundary
 * --------------------------------------------------------------------------- */
static u8 tx_buf[DMA_MAX_PKT_LEN] __attribute__((aligned(CACHE_LINE_SIZE)));
static u8 rx_buf[DMA_MAX_PKT_LEN] __attribute__((aligned(CACHE_LINE_SIZE)));

/* ---------------------------------------------------------------------------
 * Buffer descriptor storage
 * Must be aligned to XAXIDMA_BD_MINIMUM_ALIGNMENT (64 bytes)
 * --------------------------------------------------------------------------- */
#define BD_SPACE_BYTES  (DMA_BD_COUNT * XAXIDMA_BD_MINIMUM_ALIGNMENT * 2)
static u8 bd_space[BD_SPACE_BYTES] __attribute__((aligned(XAXIDMA_BD_MINIMUM_ALIGNMENT)));

/* ---------------------------------------------------------------------------
 * Status flags (set by ISR, read by main loop)
 * --------------------------------------------------------------------------- */
volatile int g_dma_tx_done = 0;
volatile int g_dma_rx_done = 0;
volatile int g_dma_error   = 0;
volatile u32 g_rx_len      = 0;

/* ---------------------------------------------------------------------------
 * Transfer statistics for periodic status reporting
 * --------------------------------------------------------------------------- */
static u32 s_tx_count = 0;
static u32 s_rx_count = 0;
static u32 s_err_count = 0;

/* ---------------------------------------------------------------------------
 * dma_init
 * Initialize AXI DMA in scatter-gather mode with MM2S and S2MM channels
 * --------------------------------------------------------------------------- */
int dma_init(void)
{
    XAxiDma_Config *cfg;
    int status;
    XAxiDma_BdRing *tx_ring, *rx_ring;
    XAxiDma_Bd bd_template;
    XAxiDma_Bd *bd_ptr;
    int bd_count;

    /* Look up hardware configuration */
    cfg = XAxiDma_LookupConfig(XPAR_AXI_DMA_0_DEVICE_ID);
    if (!cfg) {
        xil_printf("DMA: LookupConfig failed\r\n");
        return XST_FAILURE;
    }

    /* Initialize DMA engine */
    status = XAxiDma_CfgInitialize(&dma_inst, cfg);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: CfgInitialize failed %d\r\n", status);
        return status;
    }

    /* Verify scatter-gather is enabled */
    if (!XAxiDma_HasSg(&dma_inst)) {
        xil_printf("DMA: SG mode not enabled in hardware\r\n");
        return XST_FAILURE;
    }

    /* ------------------------------------------------------------------
     * Set up TX (MM2S) buffer descriptor ring
     * ------------------------------------------------------------------ */
    tx_ring = XAxiDma_GetTxRing(&dma_inst);

    /* Disable TX interrupts before setup */
    XAxiDma_BdRingIntDisable(tx_ring, XAXIDMA_IRQ_ALL_MASK);

    /* Allocate BD ring in our bd_space buffer */
    status = XAxiDma_BdRingCreate(tx_ring,
                                   (UINTPTR)bd_space,
                                   (UINTPTR)bd_space,
                                   XAXIDMA_BD_MINIMUM_ALIGNMENT,
                                   DMA_BD_COUNT);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: TX BdRingCreate failed %d\r\n", status);
        return status;
    }

    /* Clear BD template and set all fields to 0 */
    XAxiDma_BdClear(&bd_template);
    status = XAxiDma_BdRingClone(tx_ring, &bd_template);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: TX BdRingClone failed %d\r\n", status);
        return status;
    }

    /* Set coalescing: interrupt after every packet */
    status = XAxiDma_BdRingSetCoalesce(tx_ring,
                                        DMA_COALESCE_COUNT, 0xFF);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: TX SetCoalesce failed %d\r\n", status);
        return status;
    }

    /* Start TX ring */
    status = XAxiDma_BdRingStart(tx_ring);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: TX BdRingStart failed %d\r\n", status);
        return status;
    }

    /* Enable TX interrupts */
    XAxiDma_BdRingIntEnable(tx_ring, XAXIDMA_IRQ_ALL_MASK);

    /* ------------------------------------------------------------------
     * Set up RX (S2MM) buffer descriptor ring
     * ------------------------------------------------------------------ */
    rx_ring = XAxiDma_GetRxRing(&dma_inst);

    XAxiDma_BdRingIntDisable(rx_ring, XAXIDMA_IRQ_ALL_MASK);

    /* RX BDs go in second half of bd_space */
    u32 rx_bd_offset = BD_SPACE_BYTES / 2;
    status = XAxiDma_BdRingCreate(rx_ring,
                                   (UINTPTR)(bd_space + rx_bd_offset),
                                   (UINTPTR)(bd_space + rx_bd_offset),
                                   XAXIDMA_BD_MINIMUM_ALIGNMENT,
                                   DMA_BD_COUNT);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX BdRingCreate failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdClear(&bd_template);
    status = XAxiDma_BdRingClone(rx_ring, &bd_template);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX BdRingClone failed %d\r\n", status);
        return status;
    }

    /* Pre-allocate RX BDs pointing to rx_buf */
    status = XAxiDma_BdRingAlloc(rx_ring, 1, &bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX BdRingAlloc failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)rx_buf);
    XAxiDma_BdSetLength(bd_ptr, DMA_MAX_PKT_LEN,
                         rx_ring->MaxTransferLen);
    XAxiDma_BdSetCtrl(bd_ptr, 0);
    XAxiDma_BdSetId(bd_ptr, (void *)rx_buf);

    /* Flush rx_buf from cache before DMA writes to it */
    Xil_DCacheFlushRange((UINTPTR)rx_buf, DMA_MAX_PKT_LEN);

    status = XAxiDma_BdRingToHw(rx_ring, 1, bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX BdRingToHw failed %d\r\n", status);
        return status;
    }

    status = XAxiDma_BdRingSetCoalesce(rx_ring,
                                        DMA_COALESCE_COUNT, 0xFF);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX SetCoalesce failed %d\r\n", status);
        return status;
    }

    status = XAxiDma_BdRingStart(rx_ring);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX BdRingStart failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdRingIntEnable(rx_ring, XAXIDMA_IRQ_ALL_MASK);

    xil_printf("DMA: TX ring %d BDs, RX ring %d BDs ready\r\n",
               DMA_BD_COUNT, DMA_BD_COUNT);

    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * dma_send_packet
 * Transmit one packet via MM2S channel
 * --------------------------------------------------------------------------- */
int dma_send_packet(u8 *buf, u32 len)
{
    XAxiDma_BdRing *tx_ring;
    XAxiDma_Bd *bd_ptr;
    int status;

    if (len > DMA_MAX_PKT_LEN) {
        xil_printf("DMA TX: packet too large (%lu)\r\n", len);
        return XST_FAILURE;
    }

    tx_ring = XAxiDma_GetTxRing(&dma_inst);

    /* Copy packet to aligned TX buffer */
    memcpy(tx_buf, buf, len);

    /* Flush TX buffer from cache so DMA sees correct data */
    Xil_DCacheFlushRange((UINTPTR)tx_buf, len);

    /* Allocate one BD */
    status = XAxiDma_BdRingAlloc(tx_ring, 1, &bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA TX: BdRingAlloc failed %d\r\n", status);
        return status;
    }

    /* Set up BD */
    XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)tx_buf);
    XAxiDma_BdSetLength(bd_ptr, len, tx_ring->MaxTransferLen);

    /* Set SOF and EOF for single-BD transfer */
    XAxiDma_BdSetCtrl(bd_ptr,
                       XAXIDMA_BD_CTRL_TXSOF_MASK |
                       XAXIDMA_BD_CTRL_TXEOF_MASK);
    XAxiDma_BdSetId(bd_ptr, (void *)tx_buf);

    /* Reset TX done flag */
    g_dma_tx_done = 0;

    /* Submit BD to hardware */
    status = XAxiDma_BdRingToHw(tx_ring, 1, bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA TX: BdRingToHw failed %d\r\n", status);
        XAxiDma_BdRingUnAlloc(tx_ring, 1, bd_ptr);
        return status;
    }

    s_tx_count++;
    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * dma_recv_packet
 * Read received packet from RX buffer after S2MM transfer completes
 * --------------------------------------------------------------------------- */
int dma_recv_packet(u8 *buf, u32 *len)
{
    if (!g_dma_rx_done) {
        return XST_NO_DATA;
    }

    /* Invalidate cache before reading DMA-written data */
    Xil_DCacheInvalidateRange((UINTPTR)rx_buf, DMA_MAX_PKT_LEN);

    *len = g_rx_len;
    memcpy(buf, rx_buf, *len);

    g_dma_rx_done = 0;
    s_rx_count++;

    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * dma_mm2s_isr
 * TX interrupt service routine — called when MM2S transfer completes
 * --------------------------------------------------------------------------- */
void dma_mm2s_isr(void *callback)
{
    XAxiDma *dma = (XAxiDma *)callback;
    XAxiDma_BdRing *tx_ring = XAxiDma_GetTxRing(dma);
    u32 irq_status;
    int bd_count;
    XAxiDma_Bd *bd_ptr;

    /* Read and acknowledge interrupt */
    irq_status = XAxiDma_BdRingGetIrq(tx_ring);
    XAxiDma_BdRingAckIrq(tx_ring, irq_status);

    if (irq_status & XAXIDMA_IRQ_ERROR_MASK) {
        xil_printf("DMA TX ISR: ERROR 0x%08X\r\n", irq_status);
        g_dma_error = 1;
        s_err_count++;
        return;
    }

    if (irq_status & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK)) {
        /* Free completed BDs */
        bd_count = XAxiDma_BdRingFromHw(tx_ring, XAXIDMA_ALL_BDS, &bd_ptr);
        if (bd_count > 0) {
            XAxiDma_BdRingFree(tx_ring, bd_count, bd_ptr);
        }
        g_dma_tx_done = 1;
    }
}

/* ---------------------------------------------------------------------------
 * dma_s2mm_isr
 * RX interrupt service routine — called when S2MM transfer completes
 * --------------------------------------------------------------------------- */
void dma_s2mm_isr(void *callback)
{
    XAxiDma *dma = (XAxiDma *)callback;
    XAxiDma_BdRing *rx_ring = XAxiDma_GetRxRing(dma);
    u32 irq_status;
    int bd_count;
    XAxiDma_Bd *bd_ptr;

    /* Read and acknowledge interrupt */
    irq_status = XAxiDma_BdRingGetIrq(rx_ring);
    XAxiDma_BdRingAckIrq(rx_ring, irq_status);

    if (irq_status & XAXIDMA_IRQ_ERROR_MASK) {
        xil_printf("DMA RX ISR: ERROR 0x%08X\r\n", irq_status);
        g_dma_error = 1;
        s_err_count++;
        return;
    }

    if (irq_status & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK)) {
        bd_count = XAxiDma_BdRingFromHw(rx_ring, XAXIDMA_ALL_BDS, &bd_ptr);
        if (bd_count > 0) {
            /* Get actual received length from BD status */
            g_rx_len = XAxiDma_BdGetActualLength(bd_ptr,
                            rx_ring->MaxTransferLen);
            XAxiDma_BdRingFree(rx_ring, bd_count, bd_ptr);
        }
        g_dma_rx_done = 1;
    }
}

/* ---------------------------------------------------------------------------
 * print_status
 * Print periodic statistics to UART
 * --------------------------------------------------------------------------- */
void print_status(void)
{
    xil_printf("--- Status: TX=%lu  RX=%lu  ERR=%lu ---\r\n",
               s_tx_count, s_rx_count, s_err_count);
}
