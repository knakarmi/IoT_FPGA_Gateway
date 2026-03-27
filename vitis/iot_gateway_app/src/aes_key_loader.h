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

#endif /* AES_KEY_LOADER_H */
