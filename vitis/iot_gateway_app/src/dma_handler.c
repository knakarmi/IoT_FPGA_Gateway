/* =============================================================================
 * IoT Gateway - Week 10
 * File   : dma_handler.c
 * Purpose: AXI DMA scatter-gather initialization and transfer management
 *
 * The AXI DMA is configured in scatter-gather mode:
 *   MM2S (Memory-to-Stream): reads plaintext from DDR3 -> sends to AES core
 *   S2MM (Stream-to-Memory): receives ciphertext from AES core -> writes DDR3
 *
 * UART output is quiet by default. Set DMA_VERBOSE to 1 to re-enable
 * per-transfer diagnostic prints (useful for debug only).
 * =============================================================================
 */

#include "dma_handler.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_exception.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Verbosity control
 *   0 = quiet (only init banner + errors)
 *   1 = per-transfer diagnostics (old behavior)
 * --------------------------------------------------------------------------- */
#ifndef DMA_VERBOSE
#define DMA_VERBOSE 0
#endif

#if DMA_VERBOSE
#define DMA_DBG(...)  xil_printf(__VA_ARGS__)
#else
#define DMA_DBG(...)  do { } while (0)
#endif

/* ---------------------------------------------------------------------------
 * DMA instance and shared buffers
 * --------------------------------------------------------------------------- */
XAxiDma dma_inst;

static u8 tx_buf[DMA_MAX_PKT_LEN] __attribute__((aligned(CACHE_LINE_SIZE)));
       u8 rx_buf[DMA_MAX_PKT_LEN] __attribute__((aligned(CACHE_LINE_SIZE)));

/* Buffer descriptor storage - aligned to XAXIDMA_BD_MINIMUM_ALIGNMENT (64) */
#define BD_SPACE_BYTES  (DMA_BD_COUNT * XAXIDMA_BD_MINIMUM_ALIGNMENT * 2)
static u8 bd_space[BD_SPACE_BYTES] __attribute__((aligned(XAXIDMA_BD_MINIMUM_ALIGNMENT)));

/* Status flags set by ISR / poll loop */
volatile int g_dma_tx_done = 0;
volatile int g_dma_rx_done = 0;
volatile int g_dma_error   = 0;
volatile u32 g_rx_len      = 0;

/* Transfer statistics */
static u32 s_tx_count  = 0;
static u32 s_rx_count  = 0;
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

    cfg = XAxiDma_LookupConfig(XPAR_AXI_DMA_0_DEVICE_ID);
    if (!cfg) {
        xil_printf("DMA: LookupConfig failed\r\n");
        return XST_FAILURE;
    }

    status = XAxiDma_CfgInitialize(&dma_inst, cfg);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: CfgInitialize failed %d\r\n", status);
        return status;
    }

    if (!XAxiDma_HasSg(&dma_inst)) {
        xil_printf("DMA: SG mode not enabled in hardware\r\n");
        return XST_FAILURE;
    }

    /* ---------- TX (MM2S) BD ring ---------- */
    tx_ring = XAxiDma_GetTxRing(&dma_inst);
    XAxiDma_BdRingIntDisable(tx_ring, XAXIDMA_IRQ_ALL_MASK);

    status = XAxiDma_BdRingCreate(tx_ring,
                                   (UINTPTR)bd_space,
                                   (UINTPTR)bd_space,
                                   XAXIDMA_BD_MINIMUM_ALIGNMENT,
                                   DMA_BD_COUNT);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: TX BdRingCreate failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdClear(&bd_template);
    status = XAxiDma_BdRingClone(tx_ring, &bd_template);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: TX BdRingClone failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdRingSetCoalesce(tx_ring, DMA_COALESCE_COUNT, 0xFF);
    XAxiDma_BdRingStart(tx_ring);
    XAxiDma_BdRingIntEnable(tx_ring, XAXIDMA_IRQ_ALL_MASK);

    /* ---------- RX (S2MM) BD ring ---------- */
    rx_ring = XAxiDma_GetRxRing(&dma_inst);
    XAxiDma_BdRingIntDisable(rx_ring, XAXIDMA_IRQ_ALL_MASK);

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
    XAxiDma_BdRingClone(rx_ring, &bd_template);

    /* Pre-allocate one RX BD pointing to rx_buf */
    status = XAxiDma_BdRingAlloc(rx_ring, 1, &bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA: RX BdRingAlloc failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)rx_buf);
    XAxiDma_BdSetLength(bd_ptr, DMA_MAX_PKT_LEN, rx_ring->MaxTransferLen);
    XAxiDma_BdSetCtrl(bd_ptr, 0);
    XAxiDma_BdSetId(bd_ptr, (void *)rx_buf);

    Xil_DCacheFlushRange((UINTPTR)rx_buf, DMA_MAX_PKT_LEN);

    XAxiDma_BdRingToHw(rx_ring, 1, bd_ptr);
    XAxiDma_BdRingSetCoalesce(rx_ring, DMA_COALESCE_COUNT, 0xFF);
    XAxiDma_BdRingStart(rx_ring);
    XAxiDma_BdRingIntEnable(rx_ring, XAXIDMA_IRQ_ALL_MASK);

    xil_printf("DMA: ready (MM2S IRQ=%d, S2MM IRQ=%d)\r\n",
               XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR,
               XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR);
    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * dma_send_packet - transmit one packet via MM2S channel
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

    memcpy(tx_buf, buf, len);
    Xil_DCacheFlushRange((UINTPTR)tx_buf, len);

    status = XAxiDma_BdRingAlloc(tx_ring, 1, &bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("DMA TX: BdRingAlloc failed %d\r\n", status);
        return status;
    }

    XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)tx_buf);
    XAxiDma_BdSetLength(bd_ptr, len, tx_ring->MaxTransferLen);
    XAxiDma_BdSetCtrl(bd_ptr,
                       XAXIDMA_BD_CTRL_TXSOF_MASK |
                       XAXIDMA_BD_CTRL_TXEOF_MASK);
    XAxiDma_BdSetId(bd_ptr, (void *)tx_buf);

    g_dma_tx_done = 0;

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
 * When the BD's ActualLength field is 0 (SG BD not retired with a real
 * length by the DMA), scan rx_buf from the end backwards to find the last
 * non-zero byte. Works because rx_buf is zeroed before each transfer.
 * --------------------------------------------------------------------------- */
static u32 dma_scan_rx_len(u32 expected_max)
{
    u32 i;
    for (i = expected_max; i > 0; i--) {
        if (rx_buf[i - 1] != 0x00) {
            return i;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * dma_recv_packet
 * --------------------------------------------------------------------------- */
int dma_recv_packet(u8 *buf, u32 *len)
{
    if (!g_dma_rx_done) {
        return XST_NO_DATA;
    }

    Xil_DCacheInvalidateRange((UINTPTR)rx_buf, DMA_MAX_PKT_LEN);

    if (g_rx_len == 0) {
        g_rx_len = dma_scan_rx_len(DMA_MAX_PKT_LEN);
    }

    *len = g_rx_len;
    if (*len > 0) {
        memcpy(buf, rx_buf, *len);
    }

    memset(rx_buf, 0, DMA_MAX_PKT_LEN);
    g_dma_rx_done = 0;
    s_rx_count++;

    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * dma_poll_rx
 * Polling fallback for S2MM completion - reads SR register directly.
 *
 * S2MM Status Register (offset 0x34):
 *   bit3 = IOC_Irq  (transfer complete)
 *   bit1 = Idle
 *   bit0 = Halted
 * --------------------------------------------------------------------------- */
#define DMA_S2MM_SR_OFFSET  0x34
#define DMA_S2MM_IOC_BIT    (1 << 3)
#define DMA_S2MM_ERR_BITS   (0x70)

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
            Xil_Out32(base + DMA_S2MM_SR_OFFSET, sr);

            Xil_DCacheInvalidateRange((UINTPTR)rx_ring->FirstBdAddr,
                                      rx_ring->Length);

            bd_count = XAxiDma_BdRingFromHw(rx_ring,
                                             XAXIDMA_ALL_BDS, &bd_ptr);
            if (bd_count > 0) {
                Xil_DCacheInvalidateRange((UINTPTR)bd_ptr,
                                          XAXIDMA_BD_MINIMUM_ALIGNMENT);
                g_rx_len = XAxiDma_BdGetActualLength(bd_ptr,
                               rx_ring->MaxTransferLen);

                if (g_rx_len == 0) {
                    /* Fallback to S2MM transferred-byte-count register */
                    g_rx_len = Xil_In32(base + 0x58) & 0x3FFFFFF;
                }
                XAxiDma_BdRingFree(rx_ring, bd_count, bd_ptr);
            } else {
                /* Give DMA a bit more time, then retry */
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
            DMA_DBG("DMA POLL RX: complete (SR=0x%08X, len=%lu)\r\n",
                    (unsigned int)sr, g_rx_len);
            return XST_SUCCESS;
        }

        usleep(100);
        elapsed += 100;
    }

    sr = Xil_In32(base + DMA_S2MM_SR_OFFSET);
    xil_printf("DMA POLL RX: timeout SR=0x%08X\r\n", (unsigned int)sr);
    return XST_FAILURE;
}

/* ---------------------------------------------------------------------------
 * dma_rearm_rx - submit a fresh RX BD after a completed transfer
 * --------------------------------------------------------------------------- */
int dma_rearm_rx(void)
{
    XAxiDma_BdRing *rx_ring;
    XAxiDma_Bd *bd_ptr;
    XAxiDma_Bd *retired_bd;
    int retired_count;
    int status;

    rx_ring = XAxiDma_GetRxRing(&dma_inst);

    /* Retire any completed BDs to free them back to the allocation pool.
     * Without this, BdRingAlloc fails once all 16 BDs are consumed. */
    retired_count = XAxiDma_BdRingFromHw(rx_ring,
                                          XAXIDMA_ALL_BDS, &retired_bd);

    //xil_printf("  [Debug] FromHw returned %d\r\n", retired_count);

    if (retired_count > 0) {
        XAxiDma_BdRingFree(rx_ring, retired_count, retired_bd);
    }

    status = XAxiDma_BdRingAlloc(rx_ring, 1, &bd_ptr);
    if (status != XST_SUCCESS) {
        /* Still failed - try a full reset as last resort */
        xil_printf("DMA RX rearm: BdRingAlloc failed - resetting DMA\r\n");
        XAxiDma_Reset(&dma_inst);
        while (!XAxiDma_ResetIsDone(&dma_inst)) { }
        status = XAxiDma_BdRingAlloc(rx_ring, 1, &bd_ptr);
        if (status != XST_SUCCESS) {
            xil_printf("DMA RX rearm: BdRingAlloc failed after reset\r\n");
            return status;
        }
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

    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * TX / RX interrupt service routines
 * --------------------------------------------------------------------------- */
void dma_mm2s_isr(void *callback)
{
    XAxiDma *dma = (XAxiDma *)callback;
    XAxiDma_BdRing *tx_ring = XAxiDma_GetTxRing(dma);
    u32 irq_status;
    int bd_count;
    XAxiDma_Bd *bd_ptr;

    irq_status = XAxiDma_BdRingGetIrq(tx_ring);
    XAxiDma_BdRingAckIrq(tx_ring, irq_status);

    if (irq_status & XAXIDMA_IRQ_ERROR_MASK) {
        xil_printf("DMA TX ISR: ERROR 0x%08X\r\n", irq_status);
        g_dma_error = 1;
        s_err_count++;
        return;
    }

    if (irq_status & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK)) {
        bd_count = XAxiDma_BdRingFromHw(tx_ring, XAXIDMA_ALL_BDS, &bd_ptr);
        if (bd_count > 0) {
            XAxiDma_BdRingFree(tx_ring, bd_count, bd_ptr);
        }
        g_dma_tx_done = 1;
    }
}

void dma_s2mm_isr(void *callback)
{
    XAxiDma *dma = (XAxiDma *)callback;
    XAxiDma_BdRing *rx_ring = XAxiDma_GetRxRing(dma);
    u32 irq_status;
    int bd_count;
    XAxiDma_Bd *bd_ptr;

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
            g_rx_len = XAxiDma_BdGetActualLength(bd_ptr,
                            rx_ring->MaxTransferLen);
            XAxiDma_BdRingFree(rx_ring, bd_count, bd_ptr);
        }
        g_dma_rx_done = 1;
    }
}

/* ---------------------------------------------------------------------------
 * print_status - periodic statistics for the main loop
 * --------------------------------------------------------------------------- */
void print_status(void)
{
    xil_printf("--- Status: TX=%lu  RX=%lu  ERR=%lu ---\r\n",
               s_tx_count, s_rx_count, s_err_count);
}
