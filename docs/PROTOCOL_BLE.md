# BLE protocol (app ↔ proxy)

GATT contract exposed by the ESP32 proxy. The BLE link transports GDFU commands and data; it never rewrites RF frame fields ([GDFU RF protocol](https://github.com/jbrazio/gdfu-bootloader/blob/master/docs/PROTOCOL_DFU_GFSK.md)). Component behavior: [Proxy](PROXY_SPECS.md).

## Service

- Service UUID: `7e4c2a10-8b31-4f9d-a600-000000000000`
- Advertised name: `GFSK DFU PROXY`
- Requested MTU: 247
- All multibyte values are big-endian unless stated otherwise.

UUIDs share the `7e4c2a10-8b31-4f9d-a600-00000000xxxx` base, reserved by domain:

| Domain | Range |
|---|---|
| Core | `0001`–`000f` |
| Radio | `0010`–`001f` |
| Discovery | `0020`–`002f` |
| DFU transport | `0030`–`003f` |
| File management | `0040`–`004f` |

## Characteristics

| UUID | Name | Properties | Format / semantics |
|---|---|---|---|
| `...0001` | `VERSION_CAPS` | Read | `[proto_ver:1][proxy_fw_ver:3][profiles_mask:2][max_ble_chunk:2]`. Current firmware reports protocol 1, firmware 1.0.0, mask `0x0003` (both profiles), max chunk 192. |
| `...0002` | `RELEASE_VERSION` | Read | UTF-8 release string: `YYYY.M.K` for releases, `YYYY.M.K-rcN-<short-git-hash>` for release candidates, suffixed `-dirty` when built from a dirty tree. |
| `...0010` | `RADIO_CONFIG` | Read, Write | `[profile:1][freq_hz:4][tx_power_dbm:1][bitrate_bps:4][dev_hz:4][rx_bw_hz:4]` (18 bytes). Writes honor the profile byte only; it must be `1` or `2`, and the radio reinitializes on the new profile. Other bytes are ignored. While a command is pending or running or a session is active the write is rejected with status `BUSY` and the read value is restored. |
| `...0011` | `TX_POWER` | Read, Write | One signed dBm byte, 10–22 inclusive. An out-of-range write is rejected (status error 2) and the read value is restored. A valid write reinitializes the radio and persists the value. While a command is pending or running or a session is active the write is rejected with status `BUSY` and the read value is restored. |
| `...0020` | `TARGET_DISCOVERY` | Read, Notify | `[count:1] + count × [target_id:4][profile:1][rssi:1 signed][ttl_s:2]`, up to 16 entries. Republished every second so per-entry TTLs stay current without a new beacon. |
| `...0030` | `COMMAND` | Write | `[cmd:1][payload…]` — DFU transport commands below. |
| `...0031` | `DATA_STREAM` | Write, Write Without Response | Raw firmware chunks up to 192 bytes: the init packet first, then application bytes. Backpressure is signaled through `STATUS`. |
| `...0032` | `STATUS` | Read, Notify | `[state:1][accepted_bytes:4][confirmed_bytes:4][last_error:1]` (10 bytes). |
| `...0040` | `FILE_COMMAND` | Write | `[cmd:1][payload…]` — file-management commands below. |
| `...0041` | `FILE_DATA` | Read, Write | Write: upload chunk `[offset:4][1..160 bytes]`. Read: result of the last `LIST_ENTRY` or `DOWNLOAD_READ`. |
| `...0042` | `FILE_STATUS` | Read | `[op:1][last_error:1][transferred:4][total:4]`; `op` 0 = idle, 1 = upload, 2 = download. |
| `...0043` | `FILE_STORAGE` | Read | `[total_bytes:4][used_bytes:4][file_count:2]`. |

## DFU transport commands

| Cmd | Name | Payload | Behavior |
|---:|---|---|---|
| `01` | `SELECT_TARGET` | `[target_id:4]` | Selects a discovered target that is still live (TTL check). Proxy returns to `IDLE` ready state; unknown or expired target answers error 3. |
| `02` | `START_DFU` | `[dfu_start_packet:16]` optional | Sends GDFU `START` over RF. The payload is the packed little-endian Nordic `dfu_start_packet_t` (`[update_mode:1][padding:3][sd_size:4][bl_size:4][app_size:4]`); the application size at bytes 12–15 drives the proxy's progress bar. If omitted, the proxy substitutes its own application-only probe payload. A payload above 192 bytes answers error 5. |
| `03` | `ABORT` | none | Cancels the active session or command chain and returns the proxy to `IDLE`. Accepted even while another chain is running. |
| `04` | `RESET_RADIO` | none | Reinitializes the SX1262 and clears the transfer. |
| `05` | `FINISH_DFU` | none | Sends GDFU `FINISH` over RF: postvalidation and activation on the target. |

One command chain runs at a time. A `COMMAND` write while a chain is active answers `BUSY` (the single command slot is occupied); `ABORT` is the exception and is always accepted. The `START` ACK waits 15 s because the target erases flash before acknowledging; `SET_TX_POWER` waits 10 s per attempt and `FINISH` 2 s per attempt, each up to five total transmissions.

## Status

| State | Meaning |
|---:|---|
| `0x01` | `IDLE` |
| `0x02` | `BUSY` (command chain running, or flow-control active) |
| `0x03` | `STREAMING` (normal data delivery) |
| `0x04` | `ERROR` (`last_error` carries the GDFU error code from [GDFU RF protocol](https://github.com/jbrazio/gdfu-bootloader/blob/master/docs/PROTOCOL_DFU_GFSK.md)) |

`STREAMING` is reported only after the target acknowledges the RF `SET_TX_POWER` frame; before that, init and data writes are not forwarded.

### Accepted versus confirmed bytes

`STATUS` carries two separate counts. `accepted_bytes` counts bytes the proxy buffered from `DATA_STREAM`. `confirmed_bytes` counts bytes the target has acknowledged over RF. Only `confirmed_bytes` describes data the target holds; the app must drive its flow window on `confirmed_bytes` and must never treat accepted bytes as delivered.

### Backpressure

The proxy queues at most 8 firmware chunks — 1,536 bytes at the 192-byte maximum chunk. While the queue is full, `STATUS` reports `BUSY`, additional `DATA_STREAM` writes are dropped, and the state returns to `STREAMING` once an RF ACK frees space. The RF side is stop-and-wait with one frame in flight; queue capacity and the RF window are separate limits.

## File management

The proxy exposes a LittleFS partition (label `spiffs`, 4.75 MiB) as a managed cache of firmware ZIP packages. Stored packages are **not an internal DFU source**: the DFU transport never reads them, and the app still streams the selected package over `DATA_STREAM`.

Every operation completes inside its GATT write: after a `FILE_COMMAND` or `FILE_DATA` write returns, read `FILE_STATUS` for the result (and `FILE_DATA` for list/download payloads). `last_error` is sticky until the next command — except `UPLOAD_FINISH`, which keeps chunk errors visible — so a failure in any chunk of a write-without-response sequence is reported. File commands answer `BUSY` while a DFU chain or stream is active (file I/O never overlaps a DFU session). One upload or download runs at a time. Everything in progress is aborted on BLE disconnect.

Names are 1–63 characters of `[A-Za-z0-9._-]`, must not start with `.`, and must end in `.zip` (extension match is case-insensitive).

| Cmd | Name | Payload | Result |
|---:|---|---|---|
| 1 | `LIST_ENTRY` | `[index:2]` | `FILE_DATA` = `[present:1][size:4][name]`; `present=0` ends the list |
| 2 | `UPLOAD_BEGIN` | `[size:4][crc32:4][replace:1][name_len:1][name]` | Opens a temporary file. Fails `NO_SPACE` unless the file fits (4 KiB block rounding plus 3 reserve blocks), `EXISTS` when the name exists and `replace=0` |
| 3 | `UPLOAD_FINISH` | none | Verifies length and IEEE CRC-32, validates the package, then attempts to rename the temporary file over the destination |
| 4 | `UPLOAD_ABORT` | none | Deletes the temporary file |
| 5 | `DELETE` | `[name_len:1][name]` | Removes a stored package |
| 6 | `DOWNLOAD_BEGIN` | `[name_len:1][name]` | `FILE_STATUS.total` becomes the file size |
| 7 | `DOWNLOAD_READ` | `[offset:4][len:1]` (1–160) | `FILE_DATA` = bytes (shorter at end of file) |
| 8 | `DOWNLOAD_END` | none | Closes the download |

Upload chunks must arrive in order (`offset` equals bytes received so far, 1–160 bytes each). An empty or over-160-byte chunk sets `COMMAND` (1); a wrong offset or bytes extending beyond the declared upload size set `OFFSET` (8). Either failure requires the upload to restart.

File error codes (`FILE_STATUS.last_error`): 1 `COMMAND`, 2 `BUSY`, 3 `NAME`, 4 `NO_SPACE`, 5 `EXISTS`, 6 `NOT_FOUND`, 7 `IO`, 8 `OFFSET`, 9 `CRC`, 10 `INCOMPLETE`, 11 `NO_UPLOAD`, 12 `PACKAGE` (uploaded file is not a supported GFSK firmware package; this domain is separate from the DFU/RF status codes).

Replacing a file needs room for the new copy while the old one still exists; if it does not fit, delete the old file first.

## Firmware package rules

`UPLOAD_FINISH` stores an upload only when the completed archive is a supported GFSK package; otherwise it deletes the temporary file and returns `PACKAGE` (12). A package must:

- be a structurally valid ZIP with exactly three entries: `manifest.json` plus the two files named by `manifest.application.dat_file` and `manifest.application.bin_file`. No duplicates, no extra entries;
- use distinct `dat_file` and `bin_file` names, neither equal to `manifest.json`, each a plain JSON string (no escapes) of at most 63 bytes, matched byte for byte against the entry names;
- carry all three entries stored (ZIP method 0);
- declare `device_type: 82` and `device_revision: 65535` in `manifest.json` under `manifest.application.init_packet_data`;
- have non-empty `.dat` and `.bin` entries;
- have a manifest `firmware_crc16` equal to the Nordic CRC-16/CCITT-FALSE over the exact `.bin` bytes.

Validation runs in the file worker with fixed-size buffers: no extraction, no inflation, no whole-file allocation, no dependencies. After validation, the proxy first calls `LittleFS.rename` on the temporary and destination paths. If that fails while the destination exists, it deletes the destination and retries the rename. This fallback is not atomic: if the retry fails, the old destination has already been lost and the temporary file is deleted.

## Disconnect behavior

A BLE disconnect clears the transfer state (queue, byte counters, RF session), aborts any file upload or download, and restarts advertising. A stale target session is cleared by the next fresh `START` chain.

## Security

The BLE link is unauthenticated: any client in range can connect and drive the proxy. CRCs and random IDs give integrity and isolation only — see the trust boundary in [GDFU RF protocol](https://github.com/jbrazio/gdfu-bootloader/blob/master/docs/PROTOCOL_DFU_GFSK.md).
