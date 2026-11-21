#include "gdfu_frame.h"

#include <string.h>

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | (uint64_t)read_be32(p + 4);
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void write_be64(uint8_t *p, uint64_t v) {
    write_be32(p, (uint32_t)(v >> 32));
    write_be32(p + 4, (uint32_t)v);
}

uint32_t gdfu_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

bool gdfu_frame_encode(const gdfu_frame_t *frame, uint8_t *out_buf, size_t out_cap,
                       size_t *out_len) {
    if (!frame || !out_buf || !out_len) {
        return false;
    }
    if (frame->length > GDFU_MAX_PAYLOAD) {
        return false;
    }

    size_t total_len = GDFU_HEADER_SIZE + frame->length + GDFU_CRC_SIZE;
    if (out_cap < total_len) {
        return false;
    }

    out_buf[0] = GDFU_MAGIC_0;
    out_buf[1] = GDFU_MAGIC_1;
    out_buf[2] = GDFU_MAGIC_2;
    out_buf[3] = GDFU_MAGIC_3;
    out_buf[4] = GDFU_PROTOCOL_VERSION;
    out_buf[5] = (uint8_t)frame->type;
    out_buf[6] = (uint8_t)frame->profile;
    out_buf[7] = frame->flags;

    write_be32(&out_buf[8], frame->target_id);
    write_be64(&out_buf[12], frame->session_id);
    write_be32(&out_buf[20], frame->sequence);
    write_be32(&out_buf[24], frame->offset);
    write_be16(&out_buf[28], frame->length);

    if (frame->length > 0) {
        memcpy(&out_buf[GDFU_HEADER_SIZE], frame->payload, frame->length);
    }

    uint32_t crc = gdfu_crc32(out_buf, GDFU_HEADER_SIZE + frame->length);
    write_be32(&out_buf[GDFU_HEADER_SIZE + frame->length], crc);

    *out_len = total_len;
    return true;
}

bool gdfu_frame_decode(const uint8_t *buf, size_t len, gdfu_frame_t *out_frame) {
    if (!buf || !out_frame || len < (GDFU_HEADER_SIZE + GDFU_CRC_SIZE)) {
        return false;
    }
    if (buf[0] != GDFU_MAGIC_0 || buf[1] != GDFU_MAGIC_1 || buf[2] != GDFU_MAGIC_2 ||
        buf[3] != GDFU_MAGIC_3) {
        return false;
    }
    if (buf[4] != GDFU_PROTOCOL_VERSION || buf[7] != 0u) {
        return false;
    }

    uint16_t payload_len = read_be16(&buf[28]);
    if (payload_len > GDFU_MAX_PAYLOAD) {
        return false;
    }
    if (len != (size_t)(GDFU_HEADER_SIZE + payload_len + GDFU_CRC_SIZE)) {
        return false;
    }

    uint32_t expected_crc = read_be32(&buf[len - GDFU_CRC_SIZE]);
    uint32_t computed_crc = gdfu_crc32(buf, len - GDFU_CRC_SIZE);
    if (expected_crc != computed_crc) {
        return false;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->type = (gdfu_packet_type_t)buf[5];
    out_frame->profile = (gdfu_profile_id_t)buf[6];
    out_frame->flags = buf[7];
    out_frame->target_id = read_be32(&buf[8]);
    out_frame->session_id = read_be64(&buf[12]);
    out_frame->sequence = read_be32(&buf[20]);
    out_frame->offset = read_be32(&buf[24]);
    out_frame->length = payload_len;

    if (payload_len > 0) {
        memcpy(out_frame->payload, &buf[GDFU_HEADER_SIZE], payload_len);
    }

    return true;
}
