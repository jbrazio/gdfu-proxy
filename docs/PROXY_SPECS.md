# Proxy specification (Heltec WiFi LoRa 32 V3 and TTGO LoRa32 V1.6)

Implementation boundary of the ESP32 BLE↔RF bridge. Byte layouts: [BLE protocol](PROTOCOL_BLE.md) and [GDFU RF protocol](https://github.com/jbrazio/gdfu-bootloader/blob/master/docs/PROTOCOL_DFU_GFSK.md). Lifecycle overview: [Architecture](https://github.com/jbrazio/gdfu-bootloader/blob/master/docs/ARCHITECTURE.md).

## PlatformIO environment

`platformio.ini`:

- `heltec_v3_proxy`: board `heltec_wifi_lora_32_V3` (Heltec WiFi LoRa 32 V3: ESP32-S3 + SX1262), raw SX1262 backend.
- `lora32_v1_6_proxy`: board `ttgo-lora32-v1` (LILYGO TTGO LoRa32 V1.6: ESP32 + SX1276), raw SX1276 FSK backend. SX1276 RX bandwidth is the nearest supported 200 kHz on both profiles, not the SX1262 exact 187.2 kHz.
- Matching `_debug` environments (`heltec_v3_proxy_debug`, `lora32_v1_6_proxy_debug`) add debug flags; CI builds the two release environments.
- Both use platform `espressif32@6.5.0` and Arduino framework. Both runtime-select profiles 433.175 MHz and 864.100 MHz; selected hardware radio must support its profile's band.
- Custom partition tables with a `spiffs`-labeled data partition: `partitions.csv` (Heltec, 8 MB flash: 0x4C0000 = 4.75 MiB at 0x340000) and `partitions_lora32.csv` (TTGO, 4 MB flash: 0x210000 ≈ 2.06 MiB at 0x1F0000); filesystem LittleFS.
- `-O2 -Wall -Wextra`; serial monitor 115200 on UART0 (USB CDC stays disabled so `Serial` reaches the CP210x bridge).

```sh
pio run            # build
pio run -t upload  # flash
```

CI does not build the proxy firmware.

## Responsibilities

- BLE GATT server advertising `GFSK DFU PROXY` (contract: [BLE protocol](PROTOCOL_BLE.md)).
- Raw SX1262 bridge (RF contract: [GDFU RF protocol](https://github.com/jbrazio/gdfu-bootloader/blob/master/docs/PROTOCOL_DFU_GFSK.md)).
- Target discovery, bounded queueing of BLE data, stop-and-wait RF forwarding, retries and deadlines, LittleFS package cache, OLED diagnostics.

## Radio driver

`src/sx1262.cpp` is a raw SX1262 driver — no RadioLib — issuing the same command sequence as the target's HAL: pinned GFSK modulation (80 kbit/s, ±28 kHz, RX BW 187.2 kHz on both profiles), 32-bit preamble, sync word `0x13 0x37`, the mandatory TX modulation errata workaround (register `0x0889` bit 2), RX gain boost (`0x08AC = 0x96`), and a belt-and-braces re-assert of the configuration block. Both `CalibrateImage` bands are supported; the profile byte selects 433.175 or 864.100 MHz.

RF operation is nonblocking: `sx1262_send_start` arms a transmit, `sx1262_send_poll` reports completion, and `loop()` never sits inside a blocking RF wait.

## Target discovery

Beacons are decoded and cached in a table of 16 entries with a 15 s TTL; the oldest entry is evicted when full. `TARGET_DISCOVERY` is republished every second with each entry's remaining TTL, so a BLE read never exposes an expired snapshot.

## Queue and stop-and-wait RF

The proxy queues at most 8 BLE chunks (`MAX_QUEUE_CHUNKS = 8`), 1,536 bytes at the 192-byte maximum chunk. When the queue is full, further `DATA_STREAM` writes are dropped and `STATUS` reports `BUSY`; an acknowledged frame pops the head and frees space.

The RF side is strict stop-and-wait: exactly one frame is in flight, and the next `DATA` frame is armed only after the previous one is acknowledged. Queue capacity and the RF in-flight window are separate limits.

## Command chain and transfer control

`SELECT_TARGET` validates the requested target against the live beacon table. On `START_DFU` the proxy generates a random nonzero 64-bit session ID and runs the gated chain:

1. `START` — 15 s ACK window. The proxy initializes attempt 1 and re-arms only while `attempt < GDFU_MAX_RETRIES`; `GDFU_MAX_RETRIES = 5` therefore permits five total transmissions, including the initial one, 200 ms apart after failures. The target ACKs only after erasing flash. This does not accommodate the estimated 16.7 s erase for a maximum `0xC4000` image.
2. `SET_TX_POWER` — forwards the configured TX power, 10 s window per attempt, up to five total transmissions. If it fails after the `START` ACK, the proxy sends a best-effort `ABORT` so the target does not keep its session (and the raised power) until the idle timeout.
3. `STREAMING` is reported to BLE only after the `SET_TX_POWER` ACK; before that, init and data writes are held back.
4. `DATA` — one frame at a time, 2 s per stage, five total transmissions including the initial one; a failed frame re-arms from the queue head.
5. `FINISH` — 2 s window per attempt, up to five total transmissions; success clears the transfer and returns to `IDLE`.

`ABORT` is always accepted — mid-chain it cancels the running chain and any in-flight RF op immediately, which is the recovery path for a stalled `START`. `RESET_RADIO` reinitializes the SX1262. The session expires after 30 s without target traffic (error 4) and clears the transfer. Fifty consecutive RX restart failures trigger a full radio reinitialization to recover a wedged chip.

`TX_POWER` and `RADIO_CONFIG` writes answer `BUSY` (read value restored) while a command is pending or running or a session is active, because they reinitialize the radio and the target's session power is synced only once. Radio settings (profile, TX power) persist in NVS namespace `radio`; invalid stored values are normalized to profile 1 / 10 dBm at boot.

## LittleFS package cache

File-management GATT operations run on a dedicated worker task (8 KiB stack) so deep LittleFS recursion cannot overflow the BLE stack; the GATT write blocks on a semaphore, so the documented "result ready when the write returns" holds. Package validation rules, error codes, and the cache-not-a-DFU-source rule: [BLE protocol](PROTOCOL_BLE.md). File commands answer `BUSY` while a DFU chain or stream is active.

## Display and diagnostics

A 128×64 SSD1306 panel over I2C shows the state machine and selected target, the RF line (frequency, bandwidth, bitrate), queue depth and error count, and a progress bar driven by confirmed bytes. Serial logging at 115200. The display is diagnostic only and is not part of any contract.

## Recovery

- `ABORT` over BLE cancels an active transfer or chain and returns the proxy to `IDLE`.
- If the proxy stops receiving target ACKs while beacons still arrive, power-cycle it (USB re-plug); the radio reinitializes on boot. This RX degradation after long uptime has been observed on hardware.
- A stale target session is cleared by the next fresh `START` chain; if the target stays deaf, re-enter GFSK DFU on the target before judging the link dead.
