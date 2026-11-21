#include "gdfu_proxy_state.h"

#include <string.h>

void gdfu_proxy_init(gdfu_proxy_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->phase = GDFU_PROXY_IDLE;
}

void gdfu_proxy_on_beacon(gdfu_proxy_state_t *state, uint32_t target_id, uint8_t profile,
                          int8_t rssi, uint32_t now_ms) {
    if (target_id == 0u)
        return;

    /* update existing */
    for (size_t i = 0; i < state->beacon_count; ++i) {
        if (state->beacons[i].target_id == target_id) {
            state->beacons[i].profile = profile;
            state->beacons[i].rssi = rssi;
            state->beacons[i].last_seen_ms = now_ms;
            return;
        }
    }

    if (state->beacon_count < GDFU_PROXY_DISCOVERY_CAPACITY) {
        size_t idx = state->beacon_count++;
        state->beacons[idx].target_id = target_id;
        state->beacons[idx].profile = profile;
        state->beacons[idx].rssi = rssi;
        state->beacons[idx].last_seen_ms = now_ms;
        return;
    }

    /* full: evict oldest */
    size_t oldest = 0;
    for (size_t i = 1; i < state->beacon_count; ++i) {
        if (state->beacons[i].last_seen_ms < state->beacons[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    state->beacons[oldest].target_id = target_id;
    state->beacons[oldest].profile = profile;
    state->beacons[oldest].rssi = rssi;
    state->beacons[oldest].last_seen_ms = now_ms;
}

size_t gdfu_proxy_live_beacons(const gdfu_proxy_state_t *state, uint32_t now_ms) {
    size_t count = 0;
    for (size_t i = 0; i < state->beacon_count; ++i) {
        if ((now_ms - state->beacons[i].last_seen_ms) <= GDFU_PROXY_DISCOVERY_TTL_MS) {
            ++count;
        }
    }
    return count;
}

bool gdfu_proxy_select_target(gdfu_proxy_state_t *state, uint32_t target_id, uint32_t now_ms) {
    if (target_id == 0u || state->phase == GDFU_PROXY_STREAMING)
        return false;
    for (size_t i = 0; i < state->beacon_count; ++i) {
        if (state->beacons[i].target_id == target_id &&
            (now_ms - state->beacons[i].last_seen_ms) <= GDFU_PROXY_DISCOVERY_TTL_MS) {
            state->selected_target = target_id;
            state->phase = GDFU_PROXY_SELECTED;
            return true;
        }
    }
    return false;
}

bool gdfu_proxy_start_session(gdfu_proxy_state_t *state, uint64_t session_id, uint32_t now_ms) {
    if (state->phase != GDFU_PROXY_SELECTED || session_id == 0u || state->selected_target == 0u) {
        return false;
    }
    state->session_id = session_id;
    state->next_sequence = 0u;
    state->confirmed_offset = 0u;
    state->retries = 0u;
    state->last_peer_ms = now_ms;
    state->last_error = 0u;
    state->phase = GDFU_PROXY_STREAMING;
    return true;
}

bool gdfu_proxy_on_ack(gdfu_proxy_state_t *state, uint64_t session_id, uint32_t sequence,
                       uint32_t confirmed_offset, uint32_t now_ms) {
    if (state->phase != GDFU_PROXY_STREAMING || sequence != state->next_sequence ||
        session_id != state->session_id) {
        return false;
    }
    state->confirmed_offset = confirmed_offset;
    state->next_sequence++;
    state->retries = 0u;
    state->last_peer_ms = now_ms;
    return true;
}

void gdfu_proxy_on_nack(gdfu_proxy_state_t *state, uint8_t error_code) {
    state->last_error = error_code;
    state->retries = GDFU_MAX_RETRIES + 1u;
}

void gdfu_proxy_on_peer_traffic(gdfu_proxy_state_t *state, uint32_t now_ms) {
    state->last_peer_ms = now_ms;
}

bool gdfu_proxy_session_expired(const gdfu_proxy_state_t *state, uint32_t now_ms) {
    if (state->phase != GDFU_PROXY_STREAMING)
        return false;
    return (now_ms - state->last_peer_ms) > GDFU_PROXY_SESSION_TIMEOUT_MS;
}

void gdfu_proxy_abort(gdfu_proxy_state_t *state) {
    state->phase = GDFU_PROXY_IDLE;
    state->selected_target = 0u;
    state->session_id = 0u;
    state->next_sequence = 0u;
    state->confirmed_offset = 0u;
    state->retries = 0u;
    state->last_error = 0u;
}

bool gdfu_proxy_can_retry(const gdfu_proxy_state_t *state) {
    return state->retries <= GDFU_MAX_RETRIES;
}

void gdfu_proxy_note_retx(gdfu_proxy_state_t *state) { state->retries++; }

bool gdfu_proxy_tx_power_valid(uint8_t dbm) { return dbm >= GDFU_MIN_TX_POWER_DBM && dbm <= GDFU_MAX_TX_POWER_DBM; }
