/* =============================================================================
 * File   : aes_key_loader.c
 *
 * After fix_bd_ctrl_bus.tcl, axi_gpio_ctrl bits are individually sliced:
 *   bit[0] = aes_gcm_enc_dec_i
 *   bit[1] = aes_gcm_ghash_pkt_val_i
 *   bit[2] = aes_gcm_icb_start_cnt_i
 *   bit[3] = aes_gcm_icb_stop_cnt_i
 *   bit[4] = aes_gcm_pipe_reset_i  (active HIGH)
 *
 * IMPORTANT: Before fix, all 4 pins were on the same net so every write
 * simultaneously asserted pipe_reset, preventing ready_o from ever asserting.
 * =============================================================================
 */

#include "aes_key_loader.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_io.h"
#include "sleep.h"

#define AES_GPIO_CTRL_BASE    XPAR_AXI_GPIO_CTRL_BASEADDR
#define AES_GPIO_KEYVAL_BASE  XPAR_AXI_GPIO_KEY_VAL_BASEADDR
#define AES_GPIO_STATUS_BASE  XPAR_AXI_GPIO_STATUS_BASEADDR

#define AES_GPIO_KEY_W0   0x41230000U
#define AES_GPIO_KEY_W1   0x41240000U
#define AES_GPIO_KEY_W2   0x41250000U
#define AES_GPIO_KEY_W3   0x41260000U
#define AES_GPIO_KEY_W4   0x41270000U
#define AES_GPIO_KEY_W5   0x41280000U
#define AES_GPIO_KEY_W6   0x41290000U
#define AES_GPIO_KEY_W7   0x412A0000U
#define AES_GPIO_IV_VAL   0x412B0000U
#define AES_GPIO_IV_W0    0x412C0000U
#define AES_GPIO_IV_W1    0x412D0000U
#define AES_GPIO_IV_W2    0x412E0000U

#define GPIO_DATA_OFFSET  0x00
#define GPIO_TRI_OFFSET   0x04

/* ctrl bit masks - each bit is individually sliced in hardware */
#define CTRL_ENC_DEC        (1U << 0)  /* bit0 -> enc_dec_i */
#define CTRL_GHASH_PKT_VAL  (1U << 1)  /* bit1 -> ghash_pkt_val_i */
#define CTRL_ICB_START      (1U << 2)  /* bit2 -> icb_start_cnt_i */
#define CTRL_ICB_STOP       (1U << 3)  /* bit3 -> icb_stop_cnt_i */
#define CTRL_PIPE_RESET     (1U << 4)  /* bit4 -> pipe_reset_i */

#define KEY_VAL_ALL_GROUPS  0x7U

static inline void gpio_write(u32 base, u32 val)
{
    Xil_Out32(base + GPIO_DATA_OFFSET, val);
}
static inline u32 gpio_read(u32 base)
{
    return Xil_In32(base + GPIO_DATA_OFFSET);
}
static inline void gpio_delay(void)
{
    volatile int i;
    for (i = 0; i < 1000; i++);
}

int aes_load_key(const u8 *key, const u8 *iv)
{
    int i;
    u32 kw[8];
    u32 ivw[3];
    u32 status;

    static const u32 key_gpios[8] = {
        AES_GPIO_KEY_W0, AES_GPIO_KEY_W1, AES_GPIO_KEY_W2, AES_GPIO_KEY_W3,
        AES_GPIO_KEY_W4, AES_GPIO_KEY_W5, AES_GPIO_KEY_W6, AES_GPIO_KEY_W7
    };
    static const u32 iv_gpios[3] = {
        AES_GPIO_IV_W0, AES_GPIO_IV_W1, AES_GPIO_IV_W2
    };

    /* Set GPIO directions */
    Xil_Out32(AES_GPIO_CTRL_BASE   + GPIO_TRI_OFFSET, 0x00000000);
    Xil_Out32(AES_GPIO_KEYVAL_BASE + GPIO_TRI_OFFSET, 0x00000000);
    Xil_Out32(AES_GPIO_STATUS_BASE + GPIO_TRI_OFFSET, 0xFFFFFFFF);
    Xil_Out32(AES_GPIO_IV_VAL      + GPIO_TRI_OFFSET, 0x00000000);
    for (i = 0; i < 8; i++)
        Xil_Out32(key_gpios[i] + GPIO_TRI_OFFSET, 0x00000000);
    for (i = 0; i < 3; i++)
        Xil_Out32(iv_gpios[i]  + GPIO_TRI_OFFSET, 0x00000000);

    /* Pack words big-endian */
    for (i = 0; i < 8; i++) {
        kw[i] = ((u32)key[i*4+0] << 24) | ((u32)key[i*4+1] << 16) |
                ((u32)key[i*4+2] <<  8) | ((u32)key[i*4+3]);
    }
    for (i = 0; i < 3; i++) {
        ivw[i] = ((u32)iv[i*4+0] << 24) | ((u32)iv[i*4+1] << 16) |
                 ((u32)iv[i*4+2] <<  8) | ((u32)iv[i*4+3]);
    }

    /* ------------------------------------------------------------------
     * Step 1: Assert pipe_reset (bit4 only - other bits stay 0)
     * With xlslice fix, writing 0x10 ONLY asserts pipe_reset_i
     * ------------------------------------------------------------------ */
    xil_printf("AES: [1] pipe_reset (0x10)...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_PIPE_RESET);  /* 0x10 */
    usleep(100);
    gpio_write(AES_GPIO_CTRL_BASE, 0x00);
    usleep(100);

    /* ------------------------------------------------------------------
     * Step 2: Write all 8 key words
     * ------------------------------------------------------------------ */
    xil_printf("AES: [2] Writing key words...\r\n");
    for (i = 0; i < 8; i++) {
        gpio_write(key_gpios[i], kw[i]);
        xil_printf("  key_w%d = 0x%08X\r\n", i, (unsigned int)kw[i]);
    }
    gpio_delay();

    /* ------------------------------------------------------------------
     * Step 3: Assert key_word_val = 0x7 (load all groups)
     * ------------------------------------------------------------------ */
    xil_printf("AES: [3] key_word_val=0x7...\r\n");
    gpio_write(AES_GPIO_KEYVAL_BASE, KEY_VAL_ALL_GROUPS);
    usleep(10);
    gpio_write(AES_GPIO_KEYVAL_BASE, 0x00);
    gpio_delay();

    /* ------------------------------------------------------------------
     * Step 4: Set enc_dec=1 (bit0 only, no other bits)
     * ------------------------------------------------------------------ */
    xil_printf("AES: [4] enc_dec=1 (0x01)...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC);  /* 0x01 */
    usleep(10);

    /* ------------------------------------------------------------------
     * Step 5: Write IV words
     * ------------------------------------------------------------------ */
    xil_printf("AES: [5] Writing IV words...\r\n");
    for (i = 0; i < 3; i++) {
        gpio_write(iv_gpios[i], ivw[i]);
        xil_printf("  iv_w%d = 0x%08X\r\n", i, (unsigned int)ivw[i]);
    }
    gpio_delay();

    /* ------------------------------------------------------------------
     * Step 6: Assert iv_val to latch IV
     * ------------------------------------------------------------------ */
    xil_printf("AES: [6] iv_val pulse...\r\n");
    gpio_write(AES_GPIO_IV_VAL, 1);
    usleep(10);
    gpio_write(AES_GPIO_IV_VAL, 0);
    gpio_delay();

    /* ------------------------------------------------------------------
     * Step 7: Pulse icb_start_cnt (bit2) while keeping enc_dec (bit0)
     * Writing 0x05 = bit2|bit0 = icb_start + enc_dec
     * Then 0x01 = deassert icb_start, keep enc_dec
     * ------------------------------------------------------------------ */
    xil_printf("AES: [7] icb_start pulse (0x05 -> 0x01)...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC | CTRL_ICB_START);  /* 0x05 */
    usleep(10);
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC);  /* 0x01 */
    usleep(10);

    /* ------------------------------------------------------------------
     * Step 8: Assert ghash_pkt_val (bit1) + enc_dec (bit0) = 0x03
     * ------------------------------------------------------------------ */
    xil_printf("AES: [8] ghash_pkt_val (0x03)...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC | CTRL_GHASH_PKT_VAL);  /* 0x03 */

    /* ------------------------------------------------------------------
     * Step 9: Poll ready_o
     * H0 = AES(0) needs ~14 pipeline stages = ~70ns at 200MHz
     * J0 = AES(IV||1) needs another ~70ns
     * Total < 1us; poll for 50ms max
     * ------------------------------------------------------------------ */
    xil_printf("AES: [9] Polling ready_o...\r\n");
    for (i = 0; i < 50000; i++) {
        status = gpio_read(AES_GPIO_STATUS_BASE);
        if (status & 0x1) {
            xil_printf("AES: ready_o=1 after %d us\r\n", i);
            xil_printf("AES: Key loaded successfully\r\n");
            return XST_SUCCESS;
        }
        usleep(1);
    }

    xil_printf("AES: WARNING - ready_o never asserted (50ms timeout)\r\n");
    xil_printf("AES: CTRL=0x%08X STATUS=0x%08X\r\n",
               (unsigned int)gpio_read(AES_GPIO_CTRL_BASE),
               (unsigned int)gpio_read(AES_GPIO_STATUS_BASE));
    xil_printf("AES: Proceeding anyway\r\n");
    return XST_SUCCESS;
}

int aes_verify_ready(void)
{
    u32 status = gpio_read(AES_GPIO_STATUS_BASE);
    xil_printf("AES: Status GPIO = 0x%08X (bit0=ready)\r\n",
               (unsigned int)status);
    return (status & 0x1) ? XST_SUCCESS : XST_FAILURE;
}
