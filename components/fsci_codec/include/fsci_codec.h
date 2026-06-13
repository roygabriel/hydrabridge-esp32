#ifndef HYDRA_FSCI_CODEC_H
#define HYDRA_FSCI_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* fsci_codec
 * ==========
 * FSCI (Freescale Serial Communications Interface) frame format used by
 * the myAI/Mobius BLE protocol. See docs/myai-ble-reverse-engineering.md
 * for the wire format derivation.
 *
 *   offset  size  meaning
 *   0       1     start marker  = 0x02
 *   1       1     op group
 *   2       1     op code
 *   3       2     message id    (little-endian)
 *   5       2     reserved flags
 *   7       2     payload length (little-endian)
 *   9       N     payload
 *   9+N     2     CRC16 over bytes [1 .. 8+N], little-endian
 *
 * Phase 1.1 ships the CRC primitive only. Frame builder (1.2) and
 * parser (1.3) follow.
 */

#define FSCI_START_MARKER             0x02
#define FSCI_HEADER_LEN_BYTES         9     /* start + og + oc + msgid + flags + len */
#define FSCI_TRAILER_LEN_BYTES        2     /* CRC16 LE */
#define FSCI_FRAME_OVERHEAD_BYTES     (FSCI_HEADER_LEN_BYTES + FSCI_TRAILER_LEN_BYTES)

/* Op groups (from the Java decomp's M.* constants and live captures). */
#define FSCI_OG_C2CI_REQUEST          0xde
#define FSCI_OG_C2CI_CONFIRM          0xdf

/* Op codes used for v1 control. */
#define FSCI_OC_LEGACY_GET_C2_ATTR    0x17
#define FSCI_OC_LEGACY_SET_C2_ATTR    0x18

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no input/output
 * reflection, xorout 0). The myAI protocol computes this over the FSCI
 * frame bytes starting at the op_group (offset 1) and ending at the
 * last payload byte (offset 8+N). The two-byte result is stored
 * little-endian after the payload.
 *
 * Pure C, no esp-idf dependency. Host-testable. */
uint16_t fsci_crc16(const uint8_t *data, size_t len);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t fsci_codec_init(void);
#endif

#endif /* HYDRA_FSCI_CODEC_H */
