#pragma once

#include <stddef.h>
#include <stdint.h>

#define SX1262_PIN_NC 255u

enum SX1262Status {
    SX1262_OK = 0,
    SX1262_ERR_TIMEOUT = -1,
    SX1262_ERR_BUSY = -2,
    SX1262_ERR_SPI = -3,
    SX1262_ERR_PARAM = -4,
    SX1262_ERR_CRC = -5,
};

struct SX1262Pins {
    uint8_t cs;
    uint8_t reset;
    uint8_t busy;
    uint8_t dio1;
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
};

struct SX1262Config {
    SX1262Pins pins;
    uint32_t frequency_hz;
    uint32_t rx_bandwidth_hz;
    int8_t tx_power_dbm;
};

void sx1262_set_config(const SX1262Config &config);
int sx1262_init();
/* Put the radio into sleep mode before external power is removed. */
int sx1262_sleep();
/* Non-blocking transmit: send_start arms the radio (returns immediately),
 * send_poll returns SX1262_OK on TX_DONE, SX1262_ERR_BUSY while the frame is
 * still on air, or an error. The caller owns the overall deadline. */
int sx1262_send_start(const uint8_t *buf, size_t length);
int sx1262_send_poll(void);
int sx1262_start_receive();
int sx1262_poll_irq(uint16_t *irq);
int sx1262_read_received(uint8_t *buf, size_t cap, size_t *out_len, int8_t *out_rssi);
