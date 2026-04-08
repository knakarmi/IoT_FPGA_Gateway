/* =============================================================================
 * IoT Gateway - Week 7
 * File   : dma_handler.h
 * Fix v2 : Export dma_inst so interrupt_handler can pass it as callback
 * =============================================================================
 */
#ifndef DMA_HANDLER_H
#define DMA_HANDLER_H

#include "xaxidma.h"
#include "xparameters.h"
#include "xil_types.h"

#define DMA_MAX_PKT_LEN     2048
#define DMA_BD_COUNT        16
#define DMA_COALESCE_COUNT  1
#define CACHE_LINE_SIZE     32

int  dma_init(void);
int  dma_send_packet(u8 *buf, u32 len);
int  dma_recv_packet(u8 *buf, u32 *len);
int  dma_rearm_rx(void);
int  dma_poll_rx(u32 timeout_us);
void dma_mm2s_isr(void *callback);
void dma_s2mm_isr(void *callback);
void print_status(void);

/* Exported so interrupt_handler can pass as callback pointer */
extern XAxiDma dma_inst;

/* Exported so packet_test can inspect raw ciphertext bytes directly */
extern u8 rx_buf[DMA_MAX_PKT_LEN];

extern volatile int g_dma_tx_done;
extern volatile int g_dma_rx_done;
extern volatile int g_dma_error;
extern volatile u32 g_rx_len;

#endif /* DMA_HANDLER_H */
