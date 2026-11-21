import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "scripts"))

from gatt_protocol import (  # noqa: E402
    COMMAND_UUID,
    DATA_UUID,
    DISCOVERY_UUID,
    STATUS_UUID,
    GattContractError,
    bluetoothctl_scan_commands,
    controller_address,
    select_live_target,
    status_cccd_handle,
    value_handles,
)


class FlasherGattTests(unittest.TestCase):
    def test_scan_commands_selects_requested_adapter_before_scanning(self) -> None:
        """Discovery and GATT must use same Bluetooth controller."""
        self.assertEqual(
            bluetoothctl_scan_commands("68:EC:C5:8F:97:0A"),
            "select 68:EC:C5:8F:97:0A\nscan on\n",
        )

    def test_controller_address_reads_requested_hci_entry(self) -> None:
        """The named HCI adapter selects its own Bluetooth controller."""
        listing = """
hci1:   Type: Primary  Bus: USB
        BD Address: 11:22:33:44:55:66  ACL MTU: 1021:7
hci0:   Type: Primary  Bus: USB
        BD Address: 68:EC:C5:8F:97:0A  ACL MTU: 1021:7
"""
        self.assertEqual(controller_address("hci0", listing), "68:EC:C5:8F:97:0A")

    def test_value_handles_uses_uuids_not_attribute_order(self) -> None:
        """A shifted proxy attribute table must still resolve DFU characteristics."""
        listing = """
char value handle: 0x0044, uuid: 7e4c2a10-8b31-4f9d-a600-000000000020
char value handle: 0x004b, uuid: 7e4c2a10-8b31-4f9d-a600-000000000030
char value handle: 0x0051, uuid: 7e4c2a10-8b31-4f9d-a600-000000000032
char value handle: 0x0054, uuid: 7e4c2a10-8b31-4f9d-a600-000000000031
"""
        self.assertEqual(
            value_handles(listing),
            {DISCOVERY_UUID: "0x0044", COMMAND_UUID: "0x004b", STATUS_UUID: "0x0051", DATA_UUID: "0x0054"},
        )

    def test_value_handles_rejects_proxy_missing_required_characteristic(self) -> None:
        """Flashing must stop before commands if DFU service is incomplete."""
        with self.assertRaisesRegex(GattContractError, "missing required"):
            value_handles(f"char value handle: 0x0044, uuid: {DISCOVERY_UUID}")

    def test_status_cccd_finds_descriptor_owned_by_status_characteristic(self) -> None:
        """CCCD must be discovered rather than assumed adjacent to STATUS."""
        handles = {
            DISCOVERY_UUID: "0x0044",
            COMMAND_UUID: "0x004b",
            STATUS_UUID: "0x0051",
            DATA_UUID: "0x0054",
        }
        descriptors = """
handle: 0x0045, uuid: 00002902-0000-1000-8000-00805f9b34fb
handle: 0x0053, uuid: 00002902-0000-1000-8000-00805f9b34fb
"""
        self.assertEqual(status_cccd_handle(handles, descriptors), "0x0053")

    def test_select_live_target_uses_highest_nonzero_ttl(self) -> None:
        """A stale first discovery entry must not be selected for DFU."""
        discovery = bytes([
            2,
            0x00, 0x00, 0x00, 0x01, 1, 0xA0, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x02, 1, 0xB0, 0x00, 0x04,
        ])
        self.assertEqual(select_live_target(discovery), bytes.fromhex("00000002"))

    def test_select_live_target_rejects_only_expired_targets(self) -> None:
        """START must not run after discovery has no live target."""
        discovery = bytes([1, 0x00, 0x00, 0x00, 0x01, 1, 0xA0, 0x00, 0x00])
        self.assertIsNone(select_live_target(discovery))

    def test_application_transfer_bytes_pads_only_final_partial_word(self) -> None:
        """START declares exact image size; RF DATA pads final word."""
        from gatt_protocol import application_transfer_bytes

        self.assertEqual(application_transfer_bytes(193), 196)
        self.assertEqual(application_transfer_bytes(192), 192)

    def test_application_transfer_bytes_clamps_test_window_to_image(self) -> None:
        """A large test limit cannot create empty DATA frames."""
        from gatt_protocol import application_transfer_bytes

        self.assertEqual(application_transfer_bytes(193, limit_chunks=99), 196)

    def test_value_handles_parses_gatttool_attribute_output(self) -> None:
        """UUID discovery must parse command output emitted by gatttool."""
        listing = """
handle: 0x0030, char properties: 0x12, char value handle: 0x0031, uuid: 7e4c2a10-8b31-4f9d-a600-000000000020
handle: 0x0032, char properties: 0x08, char value handle: 0x0033, uuid: 7e4c2a10-8b31-4f9d-a600-000000000030
handle: 0x0034, char properties: 0x12, char value handle: 0x0035, uuid: 7e4c2a10-8b31-4f9d-a600-000000000032
handle: 0x0037, char properties: 0x0c, char value handle: 0x0038, uuid: 7e4c2a10-8b31-4f9d-a600-000000000031
"""
        self.assertEqual(value_handles(listing)[DATA_UUID], "0x0038")

    def test_status_cccd_parses_gatttool_descriptor_output(self) -> None:
        """STATUS notify setup must use gatttool's descriptor syntax."""
        handles = {
            DISCOVERY_UUID: "0x0031",
            COMMAND_UUID: "0x0033",
            STATUS_UUID: "0x0035",
            DATA_UUID: "0x0038",
        }
        descriptors = """
handle: 0x0036, uuid: 00002902-0000-1000-8000-00805f9b34fb
handle: 0x0038, uuid: 7e4c2a10-8b31-4f9d-a600-000000000031
"""
        self.assertEqual(status_cccd_handle(handles, descriptors), "0x0036")

    def test_data_payloads_pad_only_final_application_chunk(self) -> None:
        """Intermediate DATA frames stay 192 B; final word carries FF padding."""
        from gatt_protocol import application_chunks

        self.assertEqual(application_chunks(b"a" * 192 + b"b")[0], b"a" * 192)
        self.assertEqual(application_chunks(b"a" * 192 + b"b")[1], b"b\xff\xff\xff")

    def test_proxy_mac_ignores_prompt_echo_of_controller_address(self) -> None:
        """Prompt shows proxy name beside typed controller MAC; only Device lines count."""
        from gatt_protocol import proxy_address

        output = (
            "[GFSK DFU PROXY]> select 68:EC:C5:8F:97:0A\n"
            "[NEW] Device 10:51:DB:2B:06:91 GFSK DFU PROXY\n"
        )
        self.assertEqual(proxy_address(output, "GFSK DFU PROXY"), "10:51:DB:2B:06:91")
        self.assertIsNone(proxy_address("[GFSK DFU PROXY]> select 68:EC:C5:8F:97:0A\n", "GFSK DFU PROXY"))


if __name__ == "__main__":
    unittest.main()
