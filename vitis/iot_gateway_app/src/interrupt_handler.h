/* =============================================================================
 * IoT Gateway - Week 7
 * File   : interrupt_handler.h
 * =============================================================================
 */
#ifndef INTERRUPT_HANDLER_H
#define INTERRUPT_HANDLER_H

#include "xscugic.h"
#include "xparameters.h"
#include "xil_types.h"

/* IRQ IDs confirmed from xparameters.h */
#define DMA_MM2S_IRQ_ID     XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR  /* 61 */
#define DMA_S2MM_IRQ_ID     XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR  /* 62 */

/* Parser IRQ - not yet connected in block design, disabled for now */
/* #define PARSER_IRQ_ENABLED */

/* Interrupt priorities (lower value = higher priority) */
#define DMA_IRQ_PRIORITY    0xA0
#define PARSER_IRQ_PRIORITY 0xB0
#define IRQ_TRIGGER_RISING  0x3

int  interrupt_init(void);
void parser_isr(void *callback);

extern volatile int g_parser_irq_count;

#endif /* INTERRUPT_HANDLER_H */
