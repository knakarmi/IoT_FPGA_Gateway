/* =============================================================================
 * IoT Gateway - Week 7
 * File   : dma_handler.c
 * Purpose: AXI DMA scatter-gather initialization and transfer management
 *
 * The AXI DMA is configured in scatter-gather mode:
 *   MM2S (Memory-to-Stream): reads plaintext from DDR3 -> sends to AES core
 *   S2MM (Stream-to-Memory): receives ciphertext from AES core -> writes DDR3
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
XAxiDma dma_inst;

/* ---------------------------------------------------------------------------
 * TX/RX buffers - aligned to cache line boundary
 * --------------------------------------------------------------------------- */
static u8 tx_buf[DMA_MAX_PKT_LEN] __attribute__((aligned(CACHE_LINE_SIZE)));
       u8 rx_buf[DMA_MAX_PKT_LEN] __attribute__((aligned(CACHE_LINE_SIZE)));

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
    xil_printf("DMA: Base=0x%08X  MM2S_IRQ=%d  S2MM_IRQ=%d\r\n",
               (unsigned int)cfg->BaseAddr,
               XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR,
               XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR);

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
 * dma_scan_rx_len
 * When BD ActualLength is unreliable (SG BD not retired), scan rx_buf
 * from the end to find the last non-zero byte written by the DMA.
 * This works because the buffer was zeroed before the transfer and
 * the AES ciphertext will be non-zero (statistically guaranteed).
 * --------------------------------------------------------------------------- */
static u32 dma_scan_rx_len(u32 expected_max)
{
    u32 i;
    /* Scan backward from expected_max to find last written byte */
    for (i = expected_max; i > 0; i--) {
        if (rx_buf[i - 1] != 0x00) {
            return i;
        }
    }
    return 0;
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

    /* If BD-reported length is 0 (SG BD not retired in time),
     * scan the buffer directly for actual written content */
    if (g_rx_len == 0) {
        g_rx_len = dma_scan_rx_len(DMA_MAX_PKT_LEN);
        if (g_rx_len > 0) {
            xil_printf("DMA RECV: BD len=0, scanned len=%lu\r\n", g_rx_len);
        } else {
            xil_printf("DMA RECV: rx_buf appears empty after invalidate\r\n");
            xil_printf("          First 16 rx_buf bytes: ");
            u32 j;
            for (j = 0; j < 16; j++) xil_printf("%02X ", rx_buf[j]);
            xil_printf("\r\n");
        }
    }

    *len = g_rx_len;
    if (*len > 0) {
        memcpy(buf, rx_buf, *len);
    }

    /* Zero rx_buf for next transfer (so scan works correctly next time) */
    memset(rx_buf, 0, DMA_MAX_PKT_LEN);

    g_dma_rx_done = 0;
    s_rx_count++;

    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * dma_poll_rx
 * Polling fallback for S2MM completion - reads SR register directly.
 * Use when interrupts are suspect. Returns XST_SUCCESS when S2MM IOC fires.
 *
 * S2MM Status Register (offset 0x34):
 *   bit3 = IOC_Irq  (transfer complete)
 *   bit1 = Idle
 *   bit0 = Halted
 * --------------------------------------------------------------------------- */
#define DMA_S2MM_SR_OFFSET  0x34
#define DMA_S2MM_IOC_BIT    (1 << 3)
#define DMA_S2MM_ERR_BITS   (0x70)   /* bits 4,5,6 = DMAIntErr,DMASlvErr,DMADecErr */

int dma_poll_rx(u32 timeout_us)
{
    XAxiDma_BdRing *rx_ring;
    XAxiDma_Bd *bd_ptr;
    int bd_count;
    u32 sr;
    u32 base = dma_inst.RegBase;
    u32 elapsed = 0;

    rx_ring = XAxiDma_GetRxRing(&dma_inst);

    while (elapsed < timeout_us) {
        sr = Xil_In32(base + DMA_S2MM_SR_OFFSET);

        if (sr & DMA_S2MM_ERR_BITS) {
            xil_printf("DMA POLL RX: S2MM error SR=0x%08X\r\n",
                       (unsigned int)sr);
            Xil_Out32(base + DMA_S2MM_SR_OFFSET, sr);
            g_dma_error = 1;
            return XST_FAILURE;
        }

        if (sr & DMA_S2MM_IOC_BIT) {
            /* Ack IOC bit */
            Xil_Out32(base + DMA_S2MM_SR_OFFSET, sr);

            /* Invalidate cache so we read DMA-updated BD fields */
            Xil_DCacheInvalidateRange((UINTPTR)rx_ring->FirstBdAddr,
                                      rx_ring->Length);

            bd_count = XAxiDma_BdRingFromHw(rx_ring,
                                             XAXIDMA_ALL_BDS, &bd_ptr);
            if (bd_count > 0) {
                /* Invalidate the specific BD before reading length */
                Xil_DCacheInvalidateRange((UINTPTR)bd_ptr,
                                          XAXIDMA_BD_MINIMUM_ALIGNMENT);
                g_rx_len = XAxiDma_BdGetActualLength(bd_ptr,
                               rx_ring->MaxTransferLen);

                /* If BD reports 0, AES-GCM output = input + 16 byte tag */
                /* Fall back to expected length so recv can copy correctly */
                if (g_rx_len == 0) {
                    xil_printf("DMA POLL RX: BD ActualLen=0, "
                               "using MaxTransferLen fallback\r\n");
                    /* Invalidate RX buffer and try reading SR LENGTH reg */
                    /* S2MM transferred byte count: offset 0x58 */
                    g_rx_len = Xil_In32(base + 0x58) & 0x3FFFFFF;
                    if (g_rx_len == 0) {
                        xil_printf("DMA POLL RX: SR LEN reg also 0 - "
                                   "cache coherency issue likely\r\n");
                    }
                }
                XAxiDma_BdRingFree(rx_ring, bd_count, bd_ptr);
            } else {
                xil_printf("DMA POLL RX: BdRingFromHw returned 0 BDs "
                           "(BD not yet retired - possible timing issue)\r\n");
                /* Small extra delay and retry once */
                usleep(500);
                Xil_DCacheInvalidateRange((UINTPTR)rx_ring->FirstBdAddr,
                                          rx_ring->Length);
                bd_count = XAxiDma_BdRingFromHw(rx_ring,
                                                 XAXIDMA_ALL_BDS, &bd_ptr);
                if (bd_count > 0) {
                    Xil_DCacheInvalidateRange((UINTPTR)bd_ptr,
                                              XAXIDMA_BD_MINIMUM_ALIGNMENT);
                    g_rx_len = XAxiDma_BdGetActualLength(bd_ptr,
                                   rx_ring->MaxTransferLen);
                    XAxiDma_BdRingFree(rx_ring, bd_count, bd_ptr);
                }
            }

            g_dma_rx_done = 1;
            xil_printf("DMA POLL RX: complete (SR=0x%08X, len=%lu)\r\n",
                       (unsigned int)sr, g_rx_len);
            return XST_SUCCESS;
        }

        usleep(100);
        elapsed += 100;
    }

    sr = Xil_In32(base + DMA_S2MM_SR_OFFSET);
    xil_printf("DMA POLL RX: timeout SR=0x%08X base=0x%08X\r\n",
               (unsigned int)sr, (unsigned int)base);
    return XST_FAILURE;
}

/* ---------------------------------------------------------------------------
 * dma_rearm_rx
 * Re-submit a fresh RX BD to S2MM after a completed or timed-out transfer.
 * Must be called before the next dma_send_packet() to ensure S2MM has
 * a buffer descriptor ready - otherwise S2MM stalls silently.
 * --------------------------------------------------------------------------- */
int dma_rearm_rx(void)
{
    XAxiDma_BdRing *rx_ring;
    XAxiDma_Bd *bd_ptr;
    int status;

    rx_ring = XAxiDma_GetRxRing(&dma_inst);

    status = XAxiDma_BdRingAlloc(rx_ring, 1, &bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA RX rearm: BdRingAlloc failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)rx_buf);
    XAxiDma_BdSetLength(bd_ptr, DMA_MAX_PKT_LEN, rx_ring->MaxTransferLen);
    XAxiDma_BdSetCtrl(bd_ptr, 0);
    XAxiDma_BdSetId(bd_ptr, (void *)rx_buf);

    Xil_DCacheFlushRange((UINTPTR)rx_buf, DMA_MAX_PKT_LEN);

    g_dma_rx_done = 0;

    status = XAxiDma_BdRingToHw(rx_ring, 1, bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA RX rearm: BdRingToHw failed %d\r\n", status);
        XAxiDma_BdRingUnAlloc(rx_ring, 1, bd_ptr);
        return status;
    }

    xil_printf("DMA RX: re-armed, S2MM BD ready\r\n");
    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * TX interrupt service routine - called when MM2S transfer completes
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
 * RX interrupt service routine - called when S2MM transfer completes
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
