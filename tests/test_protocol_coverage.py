from pathlib import Path
import unittest

from proxy_bridge import RadioConfig

ROOT = Path(__file__).parents[1]


class ProtocolSpecificationCoverageTests(unittest.TestCase):
    def test_radio_constants_match_target_profile(self) -> None:
        """Catches proxy RF drift from the target (gdfu-bootloader src/radio/gdfu_config.h)."""
        config = (ROOT / "src" / "gdfu_config.h").read_text()
        for define in (
            "GDFU_PROTOCOL_VERSION 1u",
            "GDFU_MAX_PAYLOAD 192u",
            "GDFU_FREQUENCY_HZ 433175000u",
            "GDFU_FREQUENCY_HZ 864100000u",
            "GDFU_RX_BANDWIDTH_433_HZ 187200u",
            "GDFU_RX_BANDWIDTH_868_HZ 187200u",
            "GDFU_BITRATE_BPS 80000u",
            "GDFU_DEVIATION_HZ 28000u",
            "GDFU_PREAMBLE_BITS 32u",
        ):
            self.assertIn(f"#define {define}", config)

        main = (ROOT / "src" / "main.cpp").read_text()
        self.assertIn("864100000u", main)
        self.assertIn("GDFU_RX_BANDWIDTH_868_HZ", main)

        radio = RadioConfig()
        self.assertEqual((radio.frequency_hz, radio.bitrate_bps, radio.fdev_hz, radio.rx_bw_hz),
                         (433175000, 80000, 28000, 187200))

        self.assertIn("five total transmissions", (ROOT / "docs" / "PROXY_SPECS.md").read_text())


if __name__ == "__main__":
    unittest.main()
