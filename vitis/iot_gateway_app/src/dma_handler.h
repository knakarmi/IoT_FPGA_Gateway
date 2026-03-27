/* =============================================================================
 * IoT Gateway - Week 7
 * File   : dma_handler.h
 * Purpose: AXI DMA scatter-gather initialization and transfer management
 * =============================================================================
 */
#ifndef DMA_HANDLER_H
#define DMA_HANDLER_H

#include "xaxidma.h"
#include "xparameters.h"
#include "xil_types.h"

/* DMA buffer sizes */
#define DMA_MAX_PKT_LEN     2048    /* max packet size in bytes */
#define DMA_BD_COUNT        16      /* number of buffer descriptors */
#define DMA_COALESCE_COUNT  1       /* interrupt after every transfer */

/* Aligned buffer declarations (must be cache-line aligned for DMA) */
#define CACHE_LINE_SIZE     32

int  dma_init(void);
int  dma_send_packet(u8 *buf, u32 len);
int  dma_recv_packet(u8 *buf, u32 *len);
void dma_mm2s_isr(void *callback);
void dma_s2mm_isr(void *callback);
void print_status(void);

/* Status flags set by ISR */
extern volatile int g_dma_tx_done;
extern volatile int g_dma_rx_done;
extern volatile int g_dma_error;
extern volatile u32 g_rx_len;

#endif /* DMA_HANDLER_H */
