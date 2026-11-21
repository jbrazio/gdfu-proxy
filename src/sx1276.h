#pragma once

#include <stddef.h>
#include <stdint.h>

#define SX1276_PIN_NC 255u

enum SX1276Status {
    SX1276_OK = 0,
    SX1276_ERR_TIMEOUT = -1,
    SX1276_ERR_BUSY = -2,
    SX1276_ERR_SPI = -3,
    SX1276_ERR_PARAM = -4,
    SX1276_ERR_CRC = -5,
};

struct SX1276Pins {
    uint8_t cs;
    uint8_t reset;
    uint8_t busy;
    uint8_t dio1;
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
};

struct SX1276Config {
    SX1276Pins pins;
    uint32_t frequency_hz;
    uint32_t rx_bandwidth_hz;
    int8_t tx_power_dbm;
};

void sx1276_set_config(const SX1276Config &config);
int sx1276_init();
int sx1276_sleep();
int sx1276_send_start(const uint8_t *buf, size_t length);
int sx1276_send_poll(void);
int sx1276_start_receive();
int sx1276_poll_irq(uint16_t *irq);
int sx1276_read_received(uint8_t *buf, size_t cap, size_t *out_len, int8_t *out_rssi);
