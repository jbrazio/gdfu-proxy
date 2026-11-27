from pathlib import Path
import unittest


class ProxySourceTests(unittest.TestCase):
    def test_proxy_uses_raw_sx1262_driver(self) -> None:
        """The proxy must issue the same SX1262 commands as the target HAL."""
        root = Path(__file__).parents[1] / "src"
        source = (root / "main.cpp").read_text(encoding="utf-8")
        driver = (root / "sx1262.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "sx1262.h"', source)
        self.assertNotIn("RadioLib.h", source)
        for token in (
            "sx1262_init()",
            "0x00, 0x20,",  # 32-bit preamble
            "0x10,",        # 16-bit sync word
            "0x13, 0x37, 0x00",
            "GDFU_RX_BANDWIDTH_868_HZ ? 0x0B : 0x0A", # profile-specific RX bandwidth
            "0x08, 0x89",   # mandatory TX modulation workaround
            "0x08, 0xAC, 0x96",  # RX boosted gain
        ):
            self.assertIn(token, driver)

    def test_sx1262_gfsk_rssi_uses_rssi_sync_not_rx_status(self) -> None:
        driver = (Path(__file__).parents[1] / "src" / "sx1262.cpp").read_text(encoding="utf-8")
        self.assertIn("*out_rssi = -static_cast<int8_t>(packet_status[2] / 2);", driver)

    def test_lora32_v1_6_proxy_uses_raw_sx1276_driver(self) -> None:
        """LoRa32 V1.6 must build its SX1276 FSK backend without RadioLib."""
        root = Path(__file__).parents[1]
        config = (root / "platformio.ini").read_text(encoding="utf-8")
        source = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        driver = (root / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        self.assertIn("[env:lora32_v1_6_proxy]", config)
        self.assertIn("board = ttgo-lora32-v1", config)
        self.assertIn("board_build.partitions = partitions_lora32.csv", config)
        lora_partitions = (root / "partitions_lora32.csv").read_text(encoding="utf-8")
        self.assertIn("app0,     app,  ota_0,   0x10000,  0x1E0000", lora_partitions)
        self.assertIn("spiffs,   data, spiffs,  0x1F0000, 0x210000", lora_partitions)
        self.assertIn("[env:lora32_v1_6_proxy_debug]", config)
        self.assertIn("-DPROXY_RADIO_SX1276=1", config)
        self.assertIn('#include "sx1276.h"', source)
        self.assertNotIn("RadioLib.h", driver)
        for token in (
            "writeRegister(REG_SYNC_VALUE_1, 0x13)",
            "writeRegister(REG_SYNC_VALUE_1 + 1, 0x37)",
            "GDFU_RX_BANDWIDTH_868_HZ",
            "GDFU_BITRATE_BPS",
            "GDFU_DEVIATION_HZ",
            "REG_FIFO",
        ):
            self.assertIn(token, driver)
        self.assertEqual(
            driver.count("return hertz == GDFU_RX_BANDWIDTH_868_HZ ? 0x09 : 0x01"), 2
        )
        for token in (
            "constexpr uint8_t REG_BITRATE_MSB = 0x02;",
            "constexpr uint8_t REG_FDEV_MSB = 0x04;",
            "constexpr uint8_t REG_FRF_MSB = 0x06;",
            "constexpr uint8_t REG_PA_CONFIG = 0x09;",
            "constexpr uint8_t REG_LNA = 0x0C;",
            "constexpr uint8_t REG_RX_CONFIG = 0x0D;",
            "constexpr uint8_t REG_RSSI_CONFIG = 0x0E;",
            "constexpr uint8_t REG_RSSI_COLLISION = 0x0F;",
            "constexpr uint8_t REG_RSSI_THRESH = 0x10;",
            "constexpr uint8_t REG_RX_BW = 0x12;",
            "constexpr uint8_t REG_AFC_BW = 0x13;",
            "constexpr uint8_t REG_RSSI_VALUE = 0x11;",
        ):
            self.assertIn(token, driver)
        self.assertIn("writeRegister(REG_PREAMBLE_MSB + 1, 0x08)", driver)
        self.assertIn("writeRegister(REG_PREAMBLE_DETECT, 0xAA)", driver)
        self.assertIn("writeRegister(REG_RSSI_CONFIG, 0xD0)", driver)
        self.assertIn("writeRegister(REG_RX_CONFIG, 0x0E);", driver)
        self.assertIn("writeRegister(REG_RX_CONFIG, 0x0E | RX_CONFIG_RESTART_WITHOUT_PLL_LOCK);", driver)
        self.assertIn("writeRegister(REG_SYNC_CONFIG, 0xB1)", driver)
        # Chip whitening/CRC differ from SX1262; firmware dewhitens, GDFU CRC32 validates.
        self.assertIn("writeRegister(REG_PACKET_CONFIG_1, 0x80)", driver)

    def test_sx1276_dewhitens_sx1262_payload_in_firmware(self) -> None:
        """Hardware capture: SX1262 whitening only decodes with this PN9 state, MSB first."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        self.assertIn("void dewhiten(uint8_t *buf, size_t length)", driver)
        self.assertIn("uint16_t state = 0x111;", driver)
        self.assertIn("dewhiten(buf, length);", driver)
        self.assertIn("uint8_t whitened[255];", driver)
        self.assertIn("dewhiten(whitened, length + 2);", driver)
        self.assertNotIn("[RAW]", driver)
        self.assertNotIn("if (false)", driver)

    def test_lora32_never_drives_flash_gpio16(self) -> None:
        """ESP32-PICO-D4 wires GPIO16/17 to its in-package flash; driving 16 trips TG1WDT."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertNotIn("#define PIN_OLED_RST 16", source)
        self.assertIn("#define PIN_OLED_RST -1", source)
        init = source[source.index("void initOled()") : source.index("Wire.begin(PIN_OLED_SDA")]
        self.assertIn("#if PIN_OLED_RST >= 0", init)
        self.assertIn("#if PIN_OLED_RST >= 0\n    digitalWrite(PIN_OLED_RST, LOW);", source)
        self.assertIn("#if PIN_OLED_RST >= 0\n    gpio_hold_dis(static_cast<gpio_num_t>(PIN_OLED_RST));", source)

    def test_sx1276_rx_drains_fifo_incrementally(self) -> None:
        """RX must empty the 66 B FIFO while the frame is on air, not after PAYLOAD_READY."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        poll = driver[driver.index("int sx1276_poll_irq") : driver.index("int sx1276_read_received")]
        self.assertIn("IRQ2_FIFO_EMPTY", driver)
        self.assertIn("drainRxFifo();", poll)
        self.assertIn("rxComplete", poll)
        drain = driver[driver.index("void drainRxFifo()") : driver.index("} // namespace")]
        self.assertIn("IRQ2_FIFO_LEVEL", drain)
        self.assertIn("readBurst(REG_FIFO, rxBuf + rxGot", drain)
        self.assertIn("abortRx();", drain)  # bad length byte (noise sync) must not wedge RX
        received = driver[driver.index("int sx1276_read_received") :]
        self.assertIn("rxComplete", received)
        self.assertIn("RX_STALL_MS", (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8"))

    def test_sx1276_tx_streams_frames_larger_than_fifo(self) -> None:
        """SX1276 FIFO holds 66 B; 160 B DATA frames must be refilled on FifoLevel."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        send = driver[driver.index("int sx1276_send_start") : driver.index("int sx1276_send_poll")]
        self.assertIn("constexpr size_t FIFO_SIZE = 66;", driver)
        self.assertIn("IRQ2_FIFO_LEVEL", send)
        self.assertNotIn("writeBurst(REG_FIFO, whitened, length);", send)
        self.assertIn("SX1276_ERR_TIMEOUT", send)

    def test_lora32_oled_uses_pico_d4_i2c_pins(self) -> None:
        """I2C scan on hardware: panel ACKs 0x3C only with SDA 21 (SCL 22)."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("#define PIN_OLED_SCL 22\n#define PIN_OLED_SDA 21", source)

    def test_oled_logs_i2c_ack(self) -> None:
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        init = source[source.index("void initOled()") :]
        self.assertIn('DEBUG_SERIAL("[OLED] ack=%d\\n"', init)

    def test_proxy_logs_received_beacons(self) -> None:
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('DEBUG_SERIAL("[RX] beacon target=', source)

    def test_lora32_uses_target_gaussian_bt_half(self) -> None:
        """SX1276 must match target's GFSK Gaussian BT 0.5 pulse shaping."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        self.assertIn("constexpr uint8_t FSK_GAUSSIAN_BT_0_5 = 0x10;", driver)
        self.assertIn("void setMode(uint8_t mode) { writeRegister(REG_OP_MODE, FSK_GAUSSIAN_BT_0_5 | mode); }", driver)

    def test_sx1276_ignores_incomplete_receive_irqs(self) -> None:
        """Only a completed, timed-out, or overrun packet leaves continuous RX."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        self.assertIn("const uint8_t rx_flags1 = flags1 & IRQ1_RX_TIMEOUT;", driver)
        self.assertIn("const uint8_t rx_flags2 = (flags2 & IRQ2_FIFO_OVERRUN) | (rxComplete ? (IRQ2_PAYLOAD_READY | IRQ2_CRC_OK) : 0);", driver)
        self.assertIn("*irq = static_cast<uint16_t>(rx_flags1) << 8 | rx_flags2;", driver)
        self.assertIn('Serial.printf("[SX1276] irq=%02X/%02X\\n", flags1, flags2);', driver)

    def test_sx1276_debug_dumps_fifo_on_overrun(self) -> None:
        """Debug builds expose the raw FIFO bytes that caused an overrun."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        overrun = driver[driver.index("int sx1276_read_received") :]
        overrun = overrun[overrun.index("if (flags2 & IRQ2_FIFO_OVERRUN)") :]
        overrun = overrun[: overrun.index("return SX1276_ERR_BUSY;")]
        self.assertIn("#if DEBUG", overrun)
        self.assertIn("readBurst(REG_FIFO, dump, sizeof(dump));", overrun)
        self.assertIn('Serial.printf("[SX1276] fifo:");', overrun)

    def test_oled_push_keeps_draining_sx1276_fifo(self) -> None:
        """Blocking OLED frame push must not starve the RX FIFO drain."""
        main = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        push = main[main.index("void oledData") : main.index("const uint8_t *oledGlyph")]
        self.assertIn("sx1276_poll_irq(&irq);", push)

    def test_sx1276_tx_appends_sx1262_crc16_in_firmware(self) -> None:
        """SX1262 target expects CCITT CRC16 (init 0x1D0F) after the payload."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        self.assertIn("uint16_t crc = 0x1D0F;", driver)
        self.assertIn("sx1262Crc16(buf, length)", driver)
        self.assertIn("writeRegister(REG_PACKET_CONFIG_1, 0x00); // fixed length", driver)

    def test_sx1276_turnaround_and_crc_tail_restart(self) -> None:
        """TX waits for target RX re-arm; RX restarts after target CRC16 tail."""
        driver = (Path(__file__).parents[1] / "src" / "sx1276.cpp").read_text(encoding="utf-8")
        main = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        drain = driver[driver.index("void drainRxFifo()") : driver.index("void sx1276_set_config")]
        send = driver[driver.index("int sx1276_send_start") : driver.index("int sx1276_send_poll")]
        poll = driver[driver.index("int sx1276_send_poll") : driver.index("int sx1276_start_receive")]
        irq_poll = driver[driver.index("int sx1276_poll_irq") : driver.index("int sx1276_read_received")]
        received = driver[driver.index("int sx1276_read_received") :]
        queue = main[main.index("if (queueCount > 0 && cmdState == CMD_NONE)") : main.index("// Keep receiver active")]
        self.assertIn("if (!inRx || rxComplete)", drain)
        self.assertIn("if (!inRx)", irq_poll)
        self.assertIn("delay(4);", send)
        self.assertIn("while (!(readRegister(REG_IRQ_FLAGS_2) & IRQ2_PACKET_SENT))", poll)
        self.assertIn("delayMicroseconds(400);", received)
        self.assertIn("serviceRfOps();", queue)

    def test_heltec_wsl_v3_proxy_runs_headless(self) -> None:
        """WSL V3 has no OLED: no reset pin, and a missing panel must skip OLED init."""
        root = Path(__file__).parents[1]
        config = (root / "platformio.ini").read_text(encoding="utf-8")
        source = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("[env:heltec_wsl_v3_proxy]", config)
        self.assertIn("-DPROXY_BOARD_HELTEC_WSL_V3=1", config)
        self.assertIn("#if defined(PROXY_BOARD_HELTEC_WSL_V3)\n#define PIN_OLED_RST -1", source)
        init = source[source.index("void initOled()") : source.index("oledReady = true;")]
        self.assertIn("if (oledAck != 0) {", init)

    def test_lora32_initializes_oled(self) -> None:
        """The TTGO OLED must be initialized like every other board."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        setup = source[source.index("void setup()") : source.index("void loop()")]
        self.assertIn("    initOled();\n    DEBUG_PRINTLN(\"[BOOT] NVS\");", setup)
        self.assertNotIn("#if !defined(PROXY_RADIO_SX1276)\n    initOled();", setup)

    def test_proxy_serial_diagnostics_are_compile_time_gated(self) -> None:
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        config = (Path(__file__).parents[1] / "platformio.ini").read_text(encoding="utf-8")
        self.assertIn("#define DEBUG 0", source)
        self.assertIn("#if DEBUG\n#define DEBUG_SERIAL(...) Serial.printf(__VA_ARGS__)", source)
        self.assertIn("#if DEBUG\n    Serial.begin(115200)", source)
        self.assertNotIn("-DDEBUG=0", config)
        self.assertIn("[env:heltec_v3_proxy_debug]", config)
        self.assertIn("-DDEBUG=1", config)

    def test_proxy_oled_shows_profile_frequency_to_kilohertz(self) -> None:
        """PROFILE1's 433.175 MHz frequency must not display as 433.1 MHz."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('"%u.%03uMHZ %uKHZ"', source)
        self.assertNotIn('"%u.%03uMHZ %uKHZ %uK"', source)
        self.assertIn("(frequency / 1000u) % 1000u", source)

    def test_proxy_oled_has_no_activity_squares(self) -> None:
        """BLE/RF TX/RX blinker squares were removed; the progress bar owns the bottom row."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        for token in ("fbBlinker", "blinkActive", "BLINK_HOLD_MS", "blinkersLive", "bleTxMs", "bleRxMs", "rfTxMs", "rfRxMs", "lastAdvPulseMs"):
            self.assertNotIn(token, source)
        self.assertIn("fbBar(0, 7, 128, confirmed_bytes, session_total_bytes)", source)

    def test_radio_decodes_target_whitened_air_format(self) -> None:
        """Plain chip framing with software length-prefix and validation."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        for token in (
            "decodeAirFrame",
            "startPayload",
        ):
            self.assertIn(token, source)

    def test_proxy_syncs_target_power_before_streaming(self) -> None:
        """A target power ACK is required before the proxy can accept DFU data.

        Nonblocking refactor: the START->SET_TX_POWER->STREAMING chain lives in
        loop()'s completion block (CMD_START_WAIT/CMD_SETPOWER_WAIT) rather
        than inline in the cmd==2 branch; the ordering invariant is unchanged.
        """
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        start_wait = source.index("case CMD_START_WAIT:")
        finish_wait = source.index("case CMD_FINISH_WAIT:", start_wait)
        flow = source[start_wait:finish_wait]
        power = flow.index("sendGdfuFrame(GDFU_PKT_SET_TX_POWER")
        streaming = flow.index("publishStatus(STATUS_STREAMING)")
        self.assertIn("&txPowerDbm, 1", flow)
        self.assertLess(power, streaming)
        # A failed power sync must tear the session down so INIT/DATA writes
        # are rejected by the phase gate, not streamed at unsynchronized power.
        self.assertIn("clearTransfer();", flow[flow.index("if (!ok) {", power):])

    def test_proxy_rejects_data_after_failed_power_sync(self) -> None:
        """Phase must not remain STREAMING when SET_TX_POWER fails."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        setpower = source.index("case CMD_SETPOWER_WAIT:")
        finish_wait = source.index("case CMD_FINISH_WAIT:", setpower)
        flow = source[setpower:finish_wait]
        failure = flow.index("} else {")
        # Branch body runs to the close of the if/else pair.
        body_end = flow.index("\n            }\n", failure)
        # Failure releases the target's session over RF, then clears the transfer.
        self.assertIn("abortSessionOverRf(", flow[failure:body_end])
        helper = source[source.index("static void abortSessionOverRf") :]
        helper = helper[: helper.index("\n}\n")]
        self.assertIn("sendGdfuFrame(GDFU_PKT_ABORT", helper)
        self.assertIn("clearTransfer();", helper)

    def test_radio_config_writes_rejected_during_session_and_data_held_until_power_synced(self) -> None:
        """TX_POWER/CONFIG writes mid-session desync the target; DATA waits for the SET_TX_POWER ACK."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        guard = source[source.index("static bool radioConfigBusy()") :]
        guard = guard[: guard.index("\n}\n")]
        for token in ("pendingCmd != 0", "cmdState != CMD_NONE", "proxy_state.phase == GDFU_PROXY_STREAMING"):
            self.assertIn(token, guard)
        for cls in ("class ConfigCallbacks", "class TxPowerCallbacks"):
            start = source.index(cls)
            body = source[start : source.index("\n};", start)]
            self.assertLess(body.index("radioConfigBusy()"), body.index("reinitRadio("))
            self.assertIn("publishStatus(STATUS_BUSY)", body)
        data = source[source.index("class DataCallbacks") : source.index("class CommandCallbacks")]
        self.assertIn("!powerSynced", data)
        self.assertIn("powerSynced = true;", source[source.index("case CMD_SETPOWER_WAIT:") :])
        self.assertIn("powerSynced = false;", source[source.index("void clearTransfer()") :])
        reinit = source[source.index("int reinitRadio(") :]
        self.assertLess(reinit.index("RADIO_INIT()"), reinit.index("rxActive = false;"))

    def test_proxy_enters_standby_after_three_second_user_button_hold(self) -> None:
        """MeshCore's active-low GPIO0 button must trigger deep standby only after a continuous hold."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        for token in (
            "PIN_USER_BUTTON 0",
            "STANDBY_HOLD_MS = 3000",
            "StandbyButton standbyButton(STANDBY_HOLD_MS)",
            "enterStandby()",
            "digitalRead(PIN_USER_BUTTON) == LOW",
            "esp_deep_sleep_start()",
        ):
            self.assertIn(token, source)
        self.assertIn("standbyButton.update(digitalRead(PIN_USER_BUTTON) == LOW, millis())", source)

    def test_proxy_releases_held_pins_after_standby_reset(self) -> None:
        """Deep-sleep GPIO holds must not leave VEXT and OLED off after reset."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        setup = source[source.index("void setup()") : source.index("void loop()")]
        for token in (
            "gpio_deep_sleep_hold_dis()",
            "gpio_hold_dis(static_cast<gpio_num_t>(PIN_NSS))",
            "gpio_hold_dis(GPIO_NUM_35)",
            "gpio_hold_dis(static_cast<gpio_num_t>(PIN_OLED_RST))",
            "gpio_hold_dis(static_cast<gpio_num_t>(PIN_VEXT))",
        ):
            self.assertIn(token, setup)

    def test_proxy_standby_disables_radio_ble_display_and_external_power(self) -> None:
        """Standby must cut active peripherals before halting the ESP32."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        driver = (Path(__file__).parents[1] / "src" / "sx1262.cpp").read_text(encoding="utf-8")
        for token in (
            "RADIO_SLEEP()",
            "BLEDevice::deinit(true)",
            "oledCommand(0xAE)",
            "Wire.end()",
            "LittleFS.end()",
            "radioPreferences.end()",
            "digitalWrite(PIN_VEXT, HIGH)",
            "esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)",
        ):
            self.assertIn(token, source)
        self.assertIn("OP_SET_SLEEP", driver)

    def test_proxy_has_bounded_rf_queue_and_ack_driven_confirmation(self) -> None:
        """Catches proxy confirming BLE data before target RF acknowledgement."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        for token in (
            "MAX_QUEUE_CHUNKS = 8",
            "RADIO_START_RECEIVE()",
            "RADIO_READ_RECEIVED(rx_buf, sizeof(rx_buf)",
            "sendGdfuFrame(GDFU_PKT_DATA",
            "confirmed_bytes += frame.length",
        ):
            self.assertIn(token, source)

    def test_proxy_exposes_full_gatt_command_and_config_interface(self) -> None:
        """Catches proxy omitting GATT characteristics required by PROTOCOL_BLE.md."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        for token in (
            "CHAR_COMMAND_UUID",
            "CHAR_CONFIG_UUID",
            "CHAR_DISCOVERY_UUID",
            "CHAR_VERSION_UUID",
            "CHAR_RELEASE_UUID",
            "PROXY_RELEASE_VERSION",
            "CHAR_DATA_UUID",
            "CHAR_STATUS_UUID",
            "gdfu_proxy_select_target",
            "gdfu_proxy_start_session",
            "GDFU_PKT_START",
            "GDFU_PKT_ABORT",
            "GDFU_PKT_BEACON",
        ):
            self.assertIn(token, source)

    def test_gatt_uuids_are_neutral_and_grouped_by_domain(self) -> None:
        """GATT UUIDs reserve contiguous ranges for each protocol domain."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        expected = {
            "SERVICE_UUID": "7e4c2a10-8b31-4f9d-a600-000000000000",
            "CHAR_VERSION_UUID": "7e4c2a10-8b31-4f9d-a600-000000000001",
            "CHAR_RELEASE_UUID": "7e4c2a10-8b31-4f9d-a600-000000000002",
            "CHAR_CONFIG_UUID": "7e4c2a10-8b31-4f9d-a600-000000000010",
            "CHAR_TX_POWER_UUID": "7e4c2a10-8b31-4f9d-a600-000000000011",
            "CHAR_DISCOVERY_UUID": "7e4c2a10-8b31-4f9d-a600-000000000020",
            "CHAR_COMMAND_UUID": "7e4c2a10-8b31-4f9d-a600-000000000030",
            "CHAR_DATA_UUID": "7e4c2a10-8b31-4f9d-a600-000000000031",
            "CHAR_STATUS_UUID": "7e4c2a10-8b31-4f9d-a600-000000000032",
        }
        for name, uuid in expected.items():
            self.assertIn(f'#define {name}', source)
            self.assertIn(uuid, source)
        self.assertNotIn("0000gdf", source.lower())

    def test_clients_keep_the_gfsk_dfu_proxy_name(self) -> None:
        """Firmware and flashers advertise and find the established proxy name."""
        root = Path(__file__).parents[1]
        proxy_name = "GFSK DFU PROXY"
        firmware = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn(f'BLEDevice::init("{proxy_name}")', firmware)
        source = (root / "scripts" / "flasher_async.py").read_text(encoding="utf-8")
        self.assertIn(f'PROXY_NAME = "{proxy_name}"', source)
        self.assertIn(f'fbTitleBar("{proxy_name}")', firmware)

    def test_proxy_keeps_receiver_active_between_beacons(self) -> None:
        """Catches short RX slices missing a target's brief GFSK preamble."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("bool rxActive = false", source)
        self.assertIn("if (!rxActive) {", source)
        self.assertIn("rxActive = true", source)
        self.assertNotIn("uint32_t slice_end = millis() + 10", source)

    def test_rx_rearm_clears_every_latched_irq_flag(self) -> None:
        """A TX interrupted mid-op (disconnect/abort/session expiry) latches TX_DONE;
        a partial clear on RX re-arm makes idle RX spin on it forever (beacons never
        decode, discovery list reads empty until reboot). Every RX entry must wipe
        the full flag set, like sx1262_init() does."""
        source = (Path(__file__).parents[1] / "src" / "sx1262.cpp").read_text(encoding="utf-8")
        start = source.index("int sx1262_start_receive()")
        body = source[start : source.index("int sx1262_read_received", start)]
        self.assertIn("clearIrq(IRQ_ALL)", body)

    def test_proxy_reserves_every_gatt_attribute_handle(self) -> None:
        """The 7-characteristic GDFU service needs 17 attributes, not the library default 15."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("createService(BLEUUID(SERVICE_UUID), 30)", source)

    def test_proxy_refreshes_discovery_ttls_without_new_beacons(self) -> None:
        """A characteristic read after expiry must not expose its last live TTL snapshot."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        loop = source[source.index("void loop()") :]
        self.assertIn("lastDiscoveryRefreshMs", loop)
        self.assertIn("publishDiscovery();", loop)

    def test_proxy_rf_transfer_is_nonblocking(self) -> None:
        """loop() must never sit inside a blocking RF TX/ACK wait."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertNotIn("sx1262_send(", source)
        self.assertIn("RADIO_SEND_START(", source)
        self.assertIn("RADIO_SEND_POLL(", source)
        self.assertIn("rfOpFailAttempt()", source)
        # Every TX-stage failure must reach the common retry/finish path;
        # otherwise a consumed TX_DONE leaves the op stuck forever.
        tx_stage = source[source.index("if (rfOpStage == RF_STAGE_TX)") : source.index("} else if (rfOpStage == RF_STAGE_ACK)")]
        self.assertIn("rfOpFailAttempt();", tx_stage)
        self.assertIn("RF_STAGE_RETRY", source)
        self.assertIn("rfOpRetryDelayUntil = millis() + 200", source)
        # No RF-wait spin loop left in main.cpp: poll-with-delay inside a while
        # was the blocking pattern (sendGdfuFrame's TXWAIT).
        self.assertNotIn("[TXWAIT]", source)

    def test_proxy_throttles_oled_i2c_during_streaming(self) -> None:
        """OLED must refresh during DFU without consuming every RF ACK window."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        update = source[source.index("void updateOled(") : source.index("void initOled()")]
        self.assertIn('#include "oled_schedule.h"', source)
        self.assertIn("oledShouldUpdate(oledReady, force, streaming, oledDirty,", update)
        self.assertNotIn("!force && proxy_state.phase == GDFU_PROXY_STREAMING", update)
        loop = source[source.index("void loop()") :]
        self.assertLess(loop.index("serviceRadioQueue();"), loop.index("updateOled();"))

    def test_proxy_reports_finish_nack_instead_of_radio_error(self) -> None:
        """A target validation NACK is a terminal protocol result, not RF loss."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        nack = source.index("if (rx_frame.type == GDFU_PKT_NACK)")
        ack_stage_end = source.index("if (rfOpKind != RF_OP_NONE", nack)
        nack_flow = source[nack:ack_stage_end]
        self.assertIn("rfOpFinish(false);", nack_flow)
        self.assertNotIn("rfOpStageDeadline = 0", nack_flow)

        finish = source.index("case CMD_FINISH_WAIT:")
        finish_flow = source[finish:source.index("case CMD_NONE:", finish)]
        self.assertIn("proxy_state.last_error == ERROR_NONE", finish_flow)
        self.assertIn("proxy_state.last_error = ERROR_RADIO", finish_flow)

    def test_proxy_defers_oled_i2c_while_radio_operation_is_active(self) -> None:
        """Synchronous OLED I2C must not delay the TX-to-RX ACK transition."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        update = source[source.index("void updateOled(") : source.index("void initOled()")]
        self.assertIn("if (rfOpKind != RF_OP_NONE)", update)

    def test_disconnect_abort_retries_over_rf(self) -> None:
        """Disconnect recovery must tolerate lost ABORT frames."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        start = source.index("if (abortOnDisconnect)")
        flow = source[start:source.index("// Nonblocking control-op completions", start)]
        self.assertIn("sendGdfuFrame(GDFU_PKT_ABORT", flow)
        self.assertIn("GDFU_MAX_RETRIES", flow)

    def test_proxy_queue_is_locked_across_ble_and_loop_tasks(self) -> None:
        """BLE onWrite push and loop() pop race on queue/queueCount; an unlocked
        shift can duplicate one chunk and drop the next (same size, bad CRC16)."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;", source)
        push = source[source.index("class DataCallbacks") : source.index("class CommandCallbacks")]
        pop = source[source.index("if (rfDataDoneOk) {") : source.index("} else {", source.index("if (rfDataDoneOk) {"))]
        clear = source[source.index("void clearTransfer()") : source.index("accepted_bytes = 0;", source.index("void clearTransfer()"))]
        for body in (push, pop, clear):
            self.assertIn("portENTER_CRITICAL(&queueMux);", body)
            self.assertIn("portEXIT_CRITICAL(&queueMux);", body)
        # Capacity check and slot claim must happen under the same lock.
        self.assertLess(push.index("portENTER_CRITICAL(&queueMux);"), push.index("queueCount >= MAX_QUEUE_CHUNKS"))

    def test_proxy_counts_missed_acks_and_retries_as_errors(self) -> None:
        """Every failed attempt (TX failure or ACK timeout) bumps the OLED ERR counter."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        fail = source[source.index("void rfOpFailAttempt()") : source.index("void serviceRfOps()")]
        self.assertLess(fail.index("errorCount++;"), fail.index("if (rfOpAttempt < rfOpMaxRetries)"))
        self.assertIn("oledDirty = true;", fail)

    def test_proxy_oled_keeps_last_session_error_after_cleanup_abort(self) -> None:
        """The app's cleanup ABORT clears last_error; the OLED must still show it until next START."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        publish = source[source.index("void publishStatus(uint8_t state)") : source.index("void publishDiscovery()")]
        self.assertIn("if (state == STATUS_ERROR)", publish)
        self.assertIn("shownError = proxy_state.last_error;", publish)
        oled = source[source.index("// State machine + target id.") : source.index("// Radio profile")]
        self.assertIn("proxy_state.last_error != 0 ? proxy_state.last_error : shownError", oled)
        loop_start = source.index("        if (cmd == 2) { // START_DFU")
        start = source[loop_start : source.index("} else if (cmd == 3) { // ABORT", loop_start)]
        self.assertIn("shownError = ERROR_NONE;", start)

    def test_proxy_preserves_target_nack_outside_finish(self) -> None:
        """START, power, and DATA failures must not mask target NACK codes as RF loss."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")

        queue = source[source.index("void serviceRadioQueue()") : source.index("class DataCallbacks")]
        data_failure = queue[queue.index("} else {") : queue.index("}\n        }\n        return;", queue.index("} else {"))]
        self.assertIn("proxy_state.last_error == ERROR_NONE", data_failure)

        start = source.index("case CMD_START_WAIT:")
        power = source.index("case CMD_SETPOWER_WAIT:", start)
        start_failure = source[source.index("if (!ok) {", start) : power]
        self.assertIn("const uint8_t error = proxy_state.last_error;", start_failure)
        self.assertLess(start_failure.index("const uint8_t error = proxy_state.last_error;"),
                        start_failure.index("clearTransfer();"))
        self.assertIn("proxy_state.last_error = error == ERROR_NONE ? ERROR_RADIO : error;", start_failure)

        finish = source.index("case CMD_FINISH_WAIT:", power)
        power_failure = source[source.index("} else {", power) : finish]
        self.assertIn("const uint8_t error = proxy_state.last_error;", power_failure)
        self.assertLess(power_failure.index("const uint8_t error = proxy_state.last_error;"),
                        power_failure.index("abortSessionOverRf("))
        self.assertIn("abortSessionOverRf(error == ERROR_NONE ? ERROR_RADIO : error);", power_failure)
        # The error survives the best-effort ABORT op and is republished on completion.
        self.assertIn("const uint8_t error = abortKeepError;", power_failure)
        self.assertIn("proxy_state.last_error = error;", power_failure)

    def test_post_start_control_frames_are_retried(self) -> None:
        """Live runs: 3 of 12 OTAs lost the first SET_TX_POWER ACK right after the
        START ACK; one attempt turned that into ERROR_RADIO (proxy error 13)."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        import re
        for pkt in ("GDFU_PKT_SET_TX_POWER",):
            call = re.search(r"sendGdfuFrame\(" + pkt + r"[^;]*\)\)", source, re.S)
            self.assertIsNotNone(call, pkt)
            self.assertNotRegex(call.group(0), r",\s*1\)\)$", pkt + " must retry")

    def test_abort_remains_available_during_command_chain(self) -> None:
        """ABORT must bypass the normal BUSY gate to recover a stalled START."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("cmdState != CMD_NONE && val[0] != 3", source)
        self.assertIn("abort during active chain; chain cancelled", source)
        cleanup = source[source.index("void cancelRfOperation()") : source.index("class ServerCallbacks")]
        self.assertIn("cmdState = CMD_NONE", cleanup)
        self.assertIn("rfDoneCmd = 0", cleanup)

    def test_command_payload_is_bounded_before_copy(self) -> None:
        """Large-MTU BLE writes must not overflow pendingPayload or rfOpPayload."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        bound = source.index("val.length() - 1 > GDFU_MAX_PAYLOAD")
        copy = source.index("pendingPayload[i]", bound)
        self.assertLess(bound, copy)

    def test_proxy_defers_rf_abort_after_disconnect(self) -> None:
        """A dropped BLE link must promptly return target to advertising."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        start = source.index("void onDisconnect(BLEServer *server) override {")
        callback = source[start : source.index("\n    }", start)]
        self.assertIn("abortOnDisconnect", callback)
        self.assertIn("volatile bool abortOnDisconnect", source)
        loop = source[source.index("void loop()") :]
        disconnect_abort = loop.index("if (abortOnDisconnect)")
        cancel = loop.index("cancelRfOperation();", disconnect_abort)
        abort = loop.index("sendGdfuFrame(GDFU_PKT_ABORT", disconnect_abort)
        clear = loop.index("clearTransfer();", abort)
        self.assertLess(cancel, abort)
        self.assertLess(abort, clear)
        self.assertIn("CMD_ABORT_WAIT", loop[abort:clear])

    def test_disconnect_leaves_filesystem_work_to_the_file_worker(self) -> None:
        """LittleFS recursion overflows the 3 KB BTC_TASK stack, so the Bluetooth
        disconnect callback may only signal the 8 KiB file worker."""
        source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        start = source.index("void onDisconnect(BLEServer *server) override {")
        callback = source[start : source.index("\n    }", start)]
        for forbidden in ("fileReset(", "LittleFS.", "fileAbortUpload(", "fileCloseDownload("):
            self.assertNotIn(forbidden, callback, f"{forbidden} must not run in onDisconnect")
        self.assertIn("fileResetPending", callback)
        worker = source[source.index("void fileWorker(void *) {") :]
        self.assertIn("fileResetPending", worker[: worker.index("\n}")])
        self.assertIn("fileReset()", worker[: worker.index("\n}")])

    def test_sx1262_driver_splits_async_send(self) -> None:
        """Driver exposes start/poll halves so callers own the deadline."""
        header = (Path(__file__).parents[1] / "src" / "sx1262.h").read_text(encoding="utf-8")
        impl = (Path(__file__).parents[1] / "src" / "sx1262.cpp").read_text(encoding="utf-8")
        self.assertIn("int sx1262_send_start(const uint8_t *buf, size_t length);", header)
        self.assertIn("int sx1262_send_poll(void);", header)
        self.assertIn("int sx1262_send_start(const uint8_t *buf, size_t length)", impl)
        self.assertIn("int sx1262_send_poll(void)", impl)
        # The old blocking entry point must be gone so nobody re-introduces it.
        self.assertNotIn("int sx1262_send(", impl)

    def test_beacon_interval_and_discovery_ttl_cover_the_beacon_period(self) -> None:
        """A live target (5 s beacons, see gdfu-bootloader) never expires between beacons."""
        src = Path(__file__).parents[1] / "src"
        self.assertIn("#define GDFU_BEACON_INTERVAL_MS 5000u", (src / "gdfu_config.h").read_text(encoding="utf-8"))
        self.assertIn("#define GDFU_PROXY_DISCOVERY_TTL_MS 15000u", (src / "gdfu_proxy_state.h").read_text(encoding="utf-8"))

    def test_proxy_retries_finish_within_target_ack_window(self) -> None:
        """Five 2 s FINISH attempts fit the target's 12 s re-ACK window (gdfu-bootloader)."""
        src = Path(__file__).parents[1] / "src"
        self.assertIn("GDFU_PKT_FINISH, nullptr, 0, GDFU_FINISH_ACK_TIMEOUT_MS", (src / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn("GDFU_FINISH_ACK_TIMEOUT_MS 2000u", (src / "gdfu_config.h").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
