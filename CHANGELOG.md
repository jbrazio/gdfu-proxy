# Changelog

All notable changes to this project are documented in this file. Format inspired by [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow CalVer `YYYY.M.K`, with `-rcN` for release candidates.

## [Unreleased]

### Added

- Three-second boot splash with the proxy name, firmware version and `by kasvoton * meshcore.pt` credit.
- TTGO LoRa32 V1.6 (SX1276) proxy target.
- Heltec Wireless Stick Lite V3 (SX1262) proxy target, `heltec_wsl_v3_proxy`. The board has no display. The proxy runs without one, and an SSD1306 wired to GPIO17 (SDA) and GPIO18 (SCL) still works.
- DFU status notifications carry an 11th byte: gFSK RSSI (dBm, signed) of the last ACK/NACK from the selected target, 0 until one arrives (real readings clamp to <= -1). Reset on target select, START_DFU and BLE disconnect. Apps that accept >= 10 bytes ignore it.
- Release assets include `<board>-<version>-merged.bin` (bootloader + partitions + app, flash at `0x0`).

### Removed

- `.zip` release assets; download the `.bin` (OTA app) or `-merged.bin` (blank chip) instead.

### Fixed

- Heltec V3 target signal strength now reports RSSI instead of a constant -1 dBm.
- OLED `ERR` counter resets when a new upload starts (START_DFU) instead of accumulating across sessions.
- BLE task and `loop()` queue handoff is now lock-protected (`portMUX_TYPE`); an unlocked push during `shift()` could duplicate one chunk and drop the next, surfacing as a CRC16 NACK at FINISH.
- OLED shows the last error even after the app's cleanup ABORT clears `last_error`; the latch clears on the next START_DFU.

### Changed

- Versions are `YYYY.M.K` for releases and `YYYY.M.K-rcN-<hash>` for pre-releases, for example `2026.11.1-rc1-a1b2c3d`. Pre-releases come from `dev`, releases from `master`.
- gFSK link is now 80 kbps / 28 kHz deviation / 187.2 kHz RX bandwidth on both profiles (was 100 kbps / 50 kHz, 234.3 kHz RX on 433 MHz); bootloader and proxy must match.
