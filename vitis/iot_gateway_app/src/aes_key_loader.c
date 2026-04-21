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

/* ============================================================================
 * GPIO base addresses - use XPAR_* canonical names from xparameters.h
 * ============================================================================ */
#define AES_GPIO_CTRL_BASE    XPAR_AXI_GPIO_CTRL_BASEADDR
#define AES_GPIO_KEYVAL_BASE  XPAR_AXI_GPIO_KEY_VAL_BASEADDR
#define AES_GPIO_STATUS_BASE  XPAR_AXI_GPIO_STATUS_BASEADDR

#define AES_GPIO_IV_W0        XPAR_AXI_GPIO_IV_BASEADDR
#define AES_GPIO_IV_W1        XPAR_AXI_GPIO_IV1_BASEADDR
#define AES_GPIO_IV_W2        XPAR_AXI_GPIO_IV2_BASEADDR

#define AES_GPIO_KEY_W0       XPAR_AXI_GPIO_KEY_W0_BASEADDR
#define AES_GPIO_KEY_W1       XPAR_AXI_GPIO_KEY_W1_BASEADDR
#define AES_GPIO_KEY_W2       XPAR_AXI_GPIO_KEY_W2_BASEADDR
#define AES_GPIO_KEY_W3       XPAR_AXI_GPIO_KEY_W3_BASEADDR
#define AES_GPIO_KEY_W4       XPAR_AXI_GPIO_KEY_W4_BASEADDR
#define AES_GPIO_KEY_W5       XPAR_AXI_GPIO_KEY_W5_BASEADDR
#define AES_GPIO_KEY_W6       XPAR_AXI_GPIO_KEY_W6_BASEADDR
#define AES_GPIO_KEY_W7       XPAR_AXI_GPIO_KEY_W7_BASEADDR

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

    /* Set GPIO directions (all outputs except STATUS which is input) */
    Xil_Out32(AES_GPIO_CTRL_BASE   + GPIO_TRI_OFFSET, 0x00000000);
    Xil_Out32(AES_GPIO_KEYVAL_BASE + GPIO_TRI_OFFSET, 0x00000000);
    Xil_Out32(AES_GPIO_STATUS_BASE + GPIO_TRI_OFFSET, 0xFFFFFFFF);
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
     * Step 3: Write all 3 IV words BEFORE pulsing key_val.
     *
     * Pulsing 0x7 on key_val therefore latches the key AND the IV in the same clock.
     * Both must be stable on their respective GPIOs when that pulse happens.
     * ------------------------------------------------------------------ */
    xil_printf("AES: [3] Writing IV words...\r\n");
    for (i = 0; i < 3; i++) {
        gpio_write(iv_gpios[i], ivw[i]);
        xil_printf("  iv_w%d = 0x%08X\r\n", i, (unsigned int)ivw[i]);
    }
    gpio_delay();

    /* ------------------------------------------------------------------
     * Step 4: Pulse key_word_val = 0x7 (also pulses iv_val simultaneously).
     * ------------------------------------------------------------------ */
    xil_printf("AES: [4] key_word_val=0x7 (also iv_val pulse)...\r\n");
    gpio_write(AES_GPIO_KEYVAL_BASE, KEY_VAL_ALL_GROUPS);
    usleep(10);
    gpio_write(AES_GPIO_KEYVAL_BASE, 0x00);
    gpio_delay();

    /* ------------------------------------------------------------------
     * Step 5: Set enc_dec=1 (bit0 only)
     * ------------------------------------------------------------------ */
    xil_printf("AES: [5] enc_dec=1 (0x01)...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC);  /* 0x01 */
    usleep(10);

    /* ------------------------------------------------------------------
     * Step 6: Pulse icb_start_cnt (bit2) while keeping enc_dec (bit0)
     *   0x05 = bit2|bit0 = icb_start + enc_dec
     *   0x01 = deassert icb_start, keep enc_dec
     * This kicks off H = AES(0) then J0 = AES(IV||1) internally.
     * ------------------------------------------------------------------ */
    xil_printf("AES: [6] icb_start pulse (0x05 -> 0x01)...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC | CTRL_ICB_START);  /* 0x05 */
    usleep(10);
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC);  /* 0x01 */
    usleep(10);

    /* ------------------------------------------------------------------
     * Step 7: Briefly assert ghash_pkt_val so the core completes
     * initialization (computes H and J0 registers). This pulse is NOT
     * the per-packet ghash_pkt_val - that's handled by aes_start_packet().
     * ------------------------------------------------------------------ */
    xil_printf("AES: [7] ghash_pkt_val (0x03) brief pulse for init...\r\n");
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC | CTRL_GHASH_PKT_VAL);
    usleep(10);
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC);  /* drop back to enc_dec only */
    usleep(10000);  /* settle before polling ready_o */

    /* ------------------------------------------------------------------
     * Step 8: Poll ready_o
     * H0 = AES(0) needs ~14 pipeline stages = ~70ns at 200MHz
     * J0 = AES(IV||1) needs another ~70ns
     * Total < 1us; poll for 50ms max
     * ------------------------------------------------------------------ */
    xil_printf("AES: [8] Polling ready_o...\r\n");
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

/* =============================================================================
 * Per-packet control helpers
 *
 * The AES-GCM core is level-sensitive on ghash_pkt_val_i:
 *   - Must be HIGH for the entire duration of a packet's data beats
 *   - Must go LOW after the last data beat to trigger EOP (tag finalize)
 *
 * The ICB counter is sticky: once icb_start_cnt_i is pulsed, the counter
 * latches "running" and auto-increments. Pulsing icb_start_cnt_i again
 * RESETS the counter to J0 starting value. That's what we want between
 * packets so each packet starts encrypting from J0+1 (fresh GHASH state).
 *
 * Sequence:
 *   aes_start_packet()
 *     -> pulse icb_start_cnt to reset counter to J0
 *     -> raise ghash_pkt_val high (stays high while data streams)
 *   [DMA transfer happens]
 *   aes_end_packet()
 *     -> drop ghash_pkt_val low (falling edge = EOP, tag finalized)
 * ============================================================================= */

void aes_start_packet(void)
{
    /* For subsequent packets under the same IV+key, we do NOT pulse
     * icb_start_cnt. Doing so was pushing new ICB blocks into an ECB
     * pipeline whose last-round output still holds an unacknowledged
     * keystream block from end-of-previous-packet, causing a wedge.
     *
     * Instead: just raise ghash_pkt_val. The counter is still running
     * (iv_val_q latched high by the initial icb_start_cnt in aes_load_key).
     * Raising ghash_pkt_val reopens the CT/AAD valid gates in
     * aes_enc_dec_ctrl, and when plaintext bytes arrive, gctr_ack pulses,
     * draining the pipeline. Normal GCM operation: counter advances
     * continuously across packets under the same IV. */
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC | CTRL_GHASH_PKT_VAL); /* 0x03 */
}

void aes_end_packet(void)
{
    /* Falling edge on ghash_pkt_val triggers EOP inside gcm_ghash:
     *   eop <= pkt_val_q and not(ghash_pkt_val_i);
     * EOP causes the core to finalize the auth tag and drive TLAST on
     * the output AXI-Stream, which closes the S2MM BD. */
    gpio_write(AES_GPIO_CTRL_BASE, CTRL_ENC_DEC);                    /* 0x01 */
    gpio_delay();
}
