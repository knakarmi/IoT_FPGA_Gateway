/* =============================================================================
 * IoT Gateway - Week 7
 * File   : aes_key_loader.c
 * Purpose: Load AES-256 key and IV into the PL encryption core via AXI GPIO
 *
 * GPIO map (from xparameters.h):
 *   XPAR_AXI_GPIO_CTRL_BASEADDR    0x41200000 -> aes_gcm_key_word_i[255:0]
 *   XPAR_AXI_GPIO_KEY_VAL_BASEADDR 0x41210000 -> aes_gcm_key_word_val_i[3:0]
 *   XPAR_AXI_GPIO_STATUS_BASEADDR  0x41220000 -> status inputs (ready, tag)
 * =============================================================================
 */

#include "aes_key_loader.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_io.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * GPIO base addresses from xparameters.h
 * --------------------------------------------------------------------------- */
#define AES_GPIO_KEY_BASE     XPAR_AXI_GPIO_CTRL_BASEADDR    /* key word data */
#define AES_GPIO_KEYVAL_BASE  XPAR_AXI_GPIO_KEY_VAL_BASEADDR /* key word strobe */
#define AES_GPIO_STATUS_BASE  XPAR_AXI_GPIO_STATUS_BASEADDR  /* status/ready */

/* AXI GPIO register offsets */
#define GPIO_DATA_OFFSET   0x00   /* Channel 1 data */
#define GPIO_TRI_OFFSET    0x04   /* Channel 1 tri-state (0=output) */
#define GPIO_DATA2_OFFSET  0x08   /* Channel 2 data */
#define GPIO_TRI2_OFFSET   0x0C   /* Channel 2 tri-state */

/* ---------------------------------------------------------------------------
 * gpio_write32 / gpio_read32 helpers
 * --------------------------------------------------------------------------- */
static void gpio_write32(u32 base, u32 offset, u32 value)
{
    Xil_Out32(base + offset, value);
}

static u32 gpio_read32(u32 base, u32 offset)
{
    return Xil_In32(base + offset);
}

/* ---------------------------------------------------------------------------
 * aes_load_key
 * Load 256-bit key (8 x 32-bit words) and 96-bit IV (3 x 32-bit words)
 * --------------------------------------------------------------------------- */
int aes_load_key(const u8 *key, const u8 *iv)
{
    int i;
    u32 word;

    /* Set all GPIO outputs (tri-state = 0 means output) */
    gpio_write32(AES_GPIO_KEY_BASE,    GPIO_TRI_OFFSET,  0x00000000);
    gpio_write32(AES_GPIO_KEYVAL_BASE, GPIO_TRI_OFFSET,  0x00000000);

    xil_printf("AES: Loading 256-bit key (8 words)...\r\n");

    /* ------------------------------------------------------------------
     * Load AES-256 key: 8 words x 32-bit = 256 bits
     * Write each word to CTRL GPIO, assert key_word_val with word index
     * ------------------------------------------------------------------ */
    for (i = 0; i < 8; i++) {
        /* Pack bytes into 32-bit word big-endian */
        word = ((u32)key[i*4+0] << 24) |
               ((u32)key[i*4+1] << 16) |
               ((u32)key[i*4+2] <<  8) |
               ((u32)key[i*4+3]);

        /* Write key word data */
        gpio_write32(AES_GPIO_KEY_BASE, GPIO_DATA_OFFSET, word);

        /* Assert key_word_val = word index (0-7) */
        gpio_write32(AES_GPIO_KEYVAL_BASE, GPIO_DATA_OFFSET, (u32)i);

        /* Small delay for PL to latch */
        volatile int d; for (d = 0; d < 200; d++);

        /* De-assert strobe */
        gpio_write32(AES_GPIO_KEYVAL_BASE, GPIO_DATA_OFFSET, 0xF);

        xil_printf("  Word[%d] = 0x%08X\r\n", i, (unsigned int)word);
    }

    /* De-assert key_word_val completely */
    gpio_write32(AES_GPIO_KEYVAL_BASE, GPIO_DATA_OFFSET, 0x0);

    xil_printf("AES: Loading 96-bit IV (3 words)...\r\n");

    /* ------------------------------------------------------------------
     * Load IV: 3 words x 32-bit = 96 bits
     * Reuse CTRL GPIO channel 2 for IV words
     * ------------------------------------------------------------------ */
    gpio_write32(AES_GPIO_KEY_BASE, GPIO_TRI2_OFFSET, 0x00000000);

    for (i = 0; i < 3; i++) {
        word = ((u32)iv[i*4+0] << 24) |
               ((u32)iv[i*4+1] << 16) |
               ((u32)iv[i*4+2] <<  8) |
               ((u32)iv[i*4+3]);

        gpio_write32(AES_GPIO_KEY_BASE, GPIO_DATA2_OFFSET, word);

        volatile int d; for (d = 0; d < 200; d++);

        xil_printf("  IV[%d]   = 0x%08X\r\n", i, (unsigned int)word);
    }

    xil_printf("AES: Key and IV loaded successfully\r\n");
    return XST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * aes_verify_ready
 * Read aes_gcm_ready_o from STATUS GPIO
 * --------------------------------------------------------------------------- */
int aes_verify_ready(void)
{
    u32 status;

    /* Set STATUS GPIO as input */
    gpio_write32(AES_GPIO_STATUS_BASE, GPIO_TRI_OFFSET, 0xFFFFFFFF);

    status = gpio_read32(AES_GPIO_STATUS_BASE, GPIO_DATA_OFFSET);
    xil_printf("AES: Status GPIO = 0x%08X (bit0=ready)\r\n",
               (unsigned int)status);

    return (status & 0x1) ? XST_SUCCESS : XST_FAILURE;
}
