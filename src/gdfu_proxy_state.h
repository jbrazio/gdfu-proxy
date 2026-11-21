#ifndef GDFU_PROXY_STATE_H
#define GDFU_PROXY_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdfu_config.h"

#define GDFU_PROXY_DISCOVERY_CAPACITY 16u
#define GDFU_PROXY_DISCOVERY_TTL_MS 15000u
#define GDFU_PROXY_SESSION_TIMEOUT_MS 30000u

typedef enum {
    GDFU_PROXY_IDLE = 0,
    GDFU_PROXY_SELECTED,
    GDFU_PROXY_STREAMING,
} gdfu_proxy_phase_t;

typedef struct {
    uint32_t target_id;
    uint8_t profile;
    int8_t rssi;
    uint32_t last_seen_ms;
} gdfu_proxy_beacon_t;

typedef struct {
    gdfu_proxy_phase_t phase;
    uint32_t selected_target;
    uint64_t session_id;
    uint32_t next_sequence;
    uint32_t confirmed_offset;
    uint8_t retries;
    uint32_t last_peer_ms;
    uint8_t last_error;
    gdfu_proxy_beacon_t beacons[GDFU_PROXY_DISCOVERY_CAPACITY];
    size_t beacon_count;
} gdfu_proxy_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void gdfu_proxy_init(gdfu_proxy_state_t *state);
void gdfu_proxy_on_beacon(gdfu_proxy_state_t *state, uint32_t target_id, uint8_t profile,
                          int8_t rssi, uint32_t now_ms);
size_t gdfu_proxy_live_beacons(const gdfu_proxy_state_t *state, uint32_t now_ms);
bool gdfu_proxy_select_target(gdfu_proxy_state_t *state, uint32_t target_id, uint32_t now_ms);
bool gdfu_proxy_start_session(gdfu_proxy_state_t *state, uint64_t session_id, uint32_t now_ms);
bool gdfu_proxy_on_ack(gdfu_proxy_state_t *state, uint64_t session_id, uint32_t sequence,
                       uint32_t confirmed_offset, uint32_t now_ms);
void gdfu_proxy_on_nack(gdfu_proxy_state_t *state, uint8_t error_code);
void gdfu_proxy_on_peer_traffic(gdfu_proxy_state_t *state, uint32_t now_ms);
bool gdfu_proxy_session_expired(const gdfu_proxy_state_t *state, uint32_t now_ms);
void gdfu_proxy_abort(gdfu_proxy_state_t *state);
bool gdfu_proxy_can_retry(const gdfu_proxy_state_t *state);
void gdfu_proxy_note_retx(gdfu_proxy_state_t *state);
bool gdfu_proxy_tx_power_valid(uint8_t dbm);

#ifdef __cplusplus
}
#endif

#endif
