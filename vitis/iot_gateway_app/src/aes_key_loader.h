/* =============================================================================
 * IoT Gateway - Week 7
 * File   : aes_key_loader.h
 * Purpose: Load AES-256 key and IV into the PL encryption core via AXI GPIO
 * =============================================================================
 */
#ifndef AES_KEY_LOADER_H
#define AES_KEY_LOADER_H

#include "xgpio.h"
#include "xparameters.h"
#include "xil_types.h"

/* AES key/IV sizes */
#define AES_KEY_SIZE_BYTES   32   /* 256-bit key */
#define AES_IV_SIZE_BYTES    12   /* 96-bit IV / nonce */

int aes_load_key(const u8 *key, const u8 *iv);
int aes_verify_ready(void);

/* ---------------------------------------------------------------------------
 * Per-packet control helpers.
 *
 * The AES-GCM core is level-sensitive on ghash_pkt_val_i:
 *   - Must be HIGH for the entire duration of a packet's data beats
 *   - Must go LOW after the last data beat to trigger EOP (tag finalize)
 *
 * Typical usage (per packet):
 *   aes_start_packet();       // pulse icb_start, raise ghash_pkt_val
 *   dma_send_packet(...);     // stream plaintext through the core
 *   wait_for_tx_done();
 *   usleep(10);               // let the 14-stage pipeline drain
 *   aes_end_packet();         // falling edge -> EOP -> TLAST
 *   wait_for_rx_done();
 * --------------------------------------------------------------------------- */
void aes_start_packet(void);
void aes_end_packet(void);

#endif /* AES_KEY_LOADER_H */
