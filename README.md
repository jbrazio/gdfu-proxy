# GDFU Proxy

## GDFU ecosystem

### About GDFU

GDFU updates firmware on nRF52840 targets over a GFSK radio link. The application connects to an ESP32 proxy over BLE. The proxy sends the firmware to the target bootloader, so the phone or computer stays near the proxy rather than the target.

### Projects

| Project | Role |
| --- | --- |
| [GDFU Application](https://github.com/jbrazio/gdfu) | Web and mobile client for discovering targets and managing firmware updates |
| [GDFU Proxy](https://github.com/jbrazio/gdfu-proxy) | ESP32 firmware bridging BLE to the GFSK radio link |
| [GDFU Bootloader](https://github.com/jbrazio/gdfu-bootloader) | nRF52840 bootloader receiving and installing target firmware |

### How it works

```text
Application ── BLE ── Proxy ── GFSK radio ── Target bootloader
```

1. Install the GDFU bootloader on a supported target and the proxy firmware on a supported ESP32 board.
2. Set the proxy and target to the same radio profile. Use hardware suited to that frequency and follow local radio regulations.
3. Put the target into DFU mode, open the application, and connect to the proxy.
4. Select the discovered target and a compatible firmware package, then start the update and monitor its progress.

## About this module

GDFU Proxy is ESP32 firmware that bridges the application's BLE GATT connection to an SX1262 or SX1276 GFSK radio. It discovers target bootloaders and relays firmware-update sessions over the radio link.

### Key features

- BLE advertising as `GFSK DFU PROXY`, with target discovery and DFU control, data, and status characteristics.
- Configurable radio profile and TX power, persisted across restarts.
- Local firmware-package storage in LittleFS with BLE file management.
- OLED display showing link state, transfer progress, and the last error.
- Deep-sleep standby by holding the user button (GPIO0) for 3 seconds.

### Supported hardware

| Board | Radio | PlatformIO environment | Validation |
| --- | --- | --- | --- |
| Heltec WiFi LoRa 32 V3 | SX1262 | `heltec_v3_proxy`, default | Hardware-validated |
| TTGO LoRa32 V1.6 | SX1276 | `lora32_v1_6_proxy` | Hardware validation not recorded |
| Seeed XIAO ESP32S3 & Wio-SX1262 Kit with 3D case | SX1262 | `xiao_s3_wio_proxy` | Hardware validation not recorded |

The XIAO kit has no display. Hold the Wio-SX1262 user button for three seconds to enter standby. If you wire an SSD1306 OLED to D4 and D5, for example through the XIAO Expansion Board, the firmware drives it.

#### Radio profiles

| Profile ID | Frequency | Default |
| --- | --- | --- |
| `1` | 433.175 MHz | Yes |
| `2` | 864.1 MHz | No |

Both profiles use GFSK at 80 kbps, 28 kHz deviation, 187.2 kHz RX bandwidth, and a 32-bit preamble. Select the proxy profile from GDFU Application; the target bootloader must be built for the same profile. Use a board and antenna suitable for the selected frequency and observe local radio regulations.

### Documentation

- [BLE protocol](docs/PROTOCOL_BLE.md).
- [GDFU RF protocol](https://github.com/jbrazio/gdfu-bootloader/blob/dev/docs/PROTOCOL_DFU_GFSK.md).

#### BLE interface

The proxy advertises the service UUID `7e4c2a10-8b31-4f9d-a600-000000000000`. Characteristics share the prefix `7e4c2a10-8b31-4f9d-a600-`; their final 12 hexadecimal digits identify these groups:

| Group | UUID suffixes | Purpose |
| --- | --- | --- |
| Core | `000000000001`, `000000000002` | Protocol capabilities and release version |
| Radio | `000000000010`, `000000000011` | Radio configuration and TX power |
| Discovery | `000000000020` | Discovered targets |
| DFU transport | `000000000030` to `000000000032` | Commands, firmware data, and session status |
| File management | `000000000040` to `000000000043` | File commands, upload data, status, and storage capacity |

See [GATT callbacks and payload handling](src/main.cpp), [file-management rules](src/gdfu_file.h), [session state](src/gdfu_proxy_state.h), and [radio settings](src/gdfu_radio_settings.h). See the application's [BLE client and parsers](https://github.com/jbrazio/gdfu/blob/dev/lib/bluetooth.ts) for the corresponding client code.

For the radio side, see the [frame definitions](src/gdfu_frame.h), [radio configuration](src/gdfu_config.h), and the target bootloader's [protocol implementation](https://github.com/jbrazio/gdfu-bootloader/tree/dev/src/radio).

### Build and flash

Requires [PlatformIO](https://platformio.org/):

```bash
pio run                                        # Heltec V3
pio run -e lora32_v1_6_proxy                    # TTGO LoRa32 V1.6
pio run -e xiao_s3_wio_proxy                    # XIAO ESP32S3 & Wio-SX1262
pio run -e heltec_v3_proxy -t upload -t monitor
```

Debug builds append `_debug` to the environment name, for example `heltec_v3_proxy_debug`. Build configuration is in [platformio.ini](platformio.ini).

Release assets per board:

- `<board>-<version>.bin` contains the application. Flash it at `0x10000` if the matching bootloader and partition table are already installed.
- `<board>-<version>-merged.bin` contains the bootloader, partition table, and application. Flash it at `0x0` on a blank chip.

```bash
esptool.py --chip esp32s3 write_flash 0x0 heltec_v3_proxy-<version>-merged.bin # Heltec V3
esptool.py --chip esp32 write_flash 0x0 lora32_v1_6_proxy-<version>-merged.bin # TTGO LoRa32
esptool.py --chip esp32s3 write_flash 0x0 xiao_s3_wio_proxy-<version>-merged.bin # XIAO ESP32S3 & Wio-SX1262
```

Replace `<version>` with the downloaded release version.

### Testing

Hardware acceptance tests are in `tests/hardware/`. Run them manually with a proxy and target connected, for example `python3 tests/hardware/test_end_to_end_ota.py`.

Run host-side unit tests without hardware:

```bash
cc -Isrc tests/test_gdfu_file.c src/gdfu_file.c src/gdfu_frame.c -o /tmp/gdfu-file-test && /tmp/gdfu-file-test
cc -Isrc tests/test_gdfu_proxy_state.c src/gdfu_proxy_state.c src/gdfu_frame.c -o /tmp/gdfu-proxy-state-test && /tmp/gdfu-proxy-state-test
cc -Isrc tests/test_gdfu_radio_settings.c src/gdfu_radio_settings.c -o /tmp/gdfu-radio-settings-test && /tmp/gdfu-radio-settings-test
c++ tests/test_oled_schedule.cpp -o /tmp/gdfu-oled-test && /tmp/gdfu-oled-test
c++ tests/test_standby_button.cpp -o /tmp/gdfu-standby-test && /tmp/gdfu-standby-test
python3 -m unittest discover -s tests -p 'test_*.py'
```

To test the complete update path, connect GDFU Application over BLE, confirm radio settings and target discovery, then transfer a compatible package to a target running GDFU Bootloader. Host tests do not validate BLE or radio behaviour on hardware.

## Versioning

Set `PROXY_RELEASE_VERSION` by hand in the `[env]` section of `platformio.ini`. Use `YYYY.M.K` for stable releases and `YYYY.M.K-rcN` for release candidates. No script or hook changes it.

Release candidate builds append the short commit hash, for example `2026.11.1-rc1-a1b2c3d`. Stable builds use the version as is. A dirty working tree adds `-dirty`. Staged, unstaged and untracked files count as dirty; ignored build output does not.

Publishing rules:

- A push to `dev` with an `-rcN` version publishes a pre-release tagged `v<version>-<hash>`, once per `rcN`. To publish another candidate, bump `rc1` to `rc2`. CI keeps every pre-release.
- A push to `master` with a stable version publishes a release tagged `v<version>`, once per version. Earlier RC tags do not block it.
- Other pushes and pull requests only build and test.
- A manual run (**Run workflow**) publishes even an already published version. It replaces the release with the same tag, so on `master` it can overwrite a production release.

To release, publish an RC on `dev`, test it, then remove the `-rcN` suffix and merge to `master`.

Changes go in the `[Unreleased]` section of [CHANGELOG.md](CHANGELOG.md).

## Licensing

Copyright (c) 2026 João Brázio. Licensed under the GNU General Public License v3.0 (GPL-3.0-only). See [LICENSE](LICENSE). This repository does not include third-party library source code. Firmware images link the PlatformIO `espressif32` framework. The Arduino-ESP32 core uses LGPL-2.1-or-later, while ESP-IDF and the bundled ESP32 BLE library use Apache-2.0.
