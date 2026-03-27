/* =============================================================================
 * IoT Gateway - Week 7
 * File   : interrupt_handler.c
 * Purpose: GIC setup for DMA MM2S and S2MM interrupts
 * =============================================================================
 */

#include "interrupt_handler.h"
#include "dma_handler.h"
#include "xil_printf.h"
#include "xil_exception.h"

static XScuGic gic_inst;
volatile int g_parser_irq_count = 0;

void parser_isr(void *callback)
{
    g_parser_irq_count++;
    xil_printf("PARSER IRQ: packet event #%d\r\n", g_parser_irq_count);
}

int interrupt_init(void)
{
    XScuGic_Config *gic_cfg;
    int status;

    gic_cfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    if (!gic_cfg) {
        xil_printf("GIC: LookupConfig failed\r\n");
        return XST_FAILURE;
    }

    status = XScuGic_CfgInitialize(&gic_inst, gic_cfg,
                                    gic_cfg->CpuBaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("GIC: CfgInitialize failed %d\r\n", status);
        return status;
    }

    /* Connect DMA MM2S interrupt */
    status = XScuGic_Connect(&gic_inst, DMA_MM2S_IRQ_ID,
                              (Xil_InterruptHandler)dma_mm2s_isr,
                              (void *)NULL);
    if (status != XST_SUCCESS) {
        xil_printf("GIC: MM2S connect failed %d\r\n", status);
        return status;
    }
    XScuGic_SetPriorityTriggerType(&gic_inst, DMA_MM2S_IRQ_ID,
                                    DMA_IRQ_PRIORITY, IRQ_TRIGGER_RISING);
    XScuGic_Enable(&gic_inst, DMA_MM2S_IRQ_ID);

    /* Connect DMA S2MM interrupt */
    status = XScuGic_Connect(&gic_inst, DMA_S2MM_IRQ_ID,
                              (Xil_InterruptHandler)dma_s2mm_isr,
                              (void *)NULL);
    if (status != XST_SUCCESS) {
        xil_printf("GIC: S2MM connect failed %d\r\n", status);
        return status;
    }
    XScuGic_SetPriorityTriggerType(&gic_inst, DMA_S2MM_IRQ_ID,
                                    DMA_IRQ_PRIORITY, IRQ_TRIGGER_RISING);
    XScuGic_Enable(&gic_inst, DMA_S2MM_IRQ_ID);

    /* Connect GIC to ARM exception handler */
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                  (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                  &gic_inst);
    Xil_ExceptionEnable();

    xil_printf("GIC: MM2S IRQ=%d, S2MM IRQ=%d ready\r\n",
               DMA_MM2S_IRQ_ID, DMA_S2MM_IRQ_ID);

    return XST_SUCCESS;
}
