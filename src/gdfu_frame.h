#ifndef GDFU_FRAME_H
#define GDFU_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdfu_config.h"

#define GDFU_MAGIC_0 'G'
#define GDFU_MAGIC_1 'D'
#define GDFU_MAGIC_2 'F'
#define GDFU_MAGIC_3 'U'
#define GDFU_HEADER_SIZE 30u
#define GDFU_CRC_SIZE 4u
#define GDFU_MAX_FRAME_SIZE (GDFU_HEADER_SIZE + GDFU_MAX_PAYLOAD + GDFU_CRC_SIZE)

typedef enum {
    GDFU_PKT_BEACON = 1,
    GDFU_PKT_START = 2,
    GDFU_PKT_READY = 3,
    GDFU_PKT_DATA = 4,
    GDFU_PKT_ACK = 5,
    GDFU_PKT_NACK = 6,
    GDFU_PKT_STATUS = 7,
    GDFU_PKT_FINISH = 8,
    GDFU_PKT_COMPLETE = 9,
    GDFU_PKT_ABORT = 10,
    GDFU_PKT_ERROR = 11,
    GDFU_PKT_SET_TX_POWER = 12,
} gdfu_packet_type_t;

typedef struct {
    gdfu_packet_type_t type;
    gdfu_profile_id_t profile;
    uint8_t flags;
    uint32_t target_id;
    uint64_t session_id;
    uint32_t sequence;
    uint32_t offset;
    uint16_t length;
    uint8_t payload[GDFU_MAX_PAYLOAD];
} gdfu_frame_t;

#ifdef __cplusplus
extern "C" {
#endif

uint32_t gdfu_crc32(const uint8_t *buf, size_t len);
bool gdfu_frame_encode(const gdfu_frame_t *frame, uint8_t *out_buf, size_t out_cap,
                       size_t *out_len);
bool gdfu_frame_decode(const uint8_t *buf, size_t len, gdfu_frame_t *out_frame);

#ifdef __cplusplus
}
#endif

#endif
