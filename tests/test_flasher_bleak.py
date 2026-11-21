import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "scripts"))

from flasher_async import BleakGatt, find_proxy_device, start_dfu_command  # noqa: E402


class FakeDevice:
    def __init__(self, name: str, address: str) -> None:
        self.name = name
        self.address = address


class FakeScanner:
    @staticmethod
    async def discover(timeout: float):
        FakeScanner.timeout = timeout
        return [FakeDevice("other", "AA"), FakeDevice("GFSK DFU PROXY", "BB")]

    @staticmethod
    async def find_device_by_filter(match, timeout: float):
        FakeScanner.filter_timeout = timeout
        for device, local_name in ((FakeDevice("other", "AA"), "other"), (FakeDevice("GFSK DFU PROXY", "BB"), "GFSK DFU PROXY")):
            adv = type("Adv", (), {"local_name": local_name})()
            if match(device, adv):
                return device
        return None


class FakeCharacteristic:
    max_write_without_response_size = 128


class FakeServices:
    @staticmethod
    def get_characteristic(uuid: str):
        return FakeCharacteristic() if uuid == "data" else None


class FakeBackend:
    def __init__(self) -> None:
        self.mtu_acquired = False

    async def _acquire_mtu(self) -> None:
        self.mtu_acquired = True


class FakeClient:
    mtu_size = 23

    def __init__(self, device) -> None:
        self.device = device
        self.writes = []
        self.callbacks = {}
        self.connected = False
        self.services = FakeServices()
        self._backend = FakeBackend()

    @property
    def is_connected(self) -> bool:
        return self.connected

    async def connect(self) -> None:
        self.connected = True

    async def disconnect(self) -> None:
        self.connected = False

    async def read_gatt_char(self, uuid: str) -> bytearray:
        return bytearray(b"\x01\x02")

    async def write_gatt_char(self, uuid: str, data: bytes, response: bool) -> None:
        self.writes.append((uuid, data, response))

    async def start_notify(self, uuid: str, callback) -> None:
        self.callbacks[uuid] = callback


class ReconnectingClient(FakeClient):
    attempts = 0

    async def connect(self) -> None:
        type(self).attempts += 1
        if type(self).attempts < 3:
            raise OSError("temporary BLE failure")
        await super().connect()


class FlasherBleakTests(unittest.IsolatedAsyncioTestCase):
    async def test_start_dfu_command_preserves_16_byte_proxy_wire_format(self) -> None:
        self.assertEqual(
            start_dfu_command(444052).hex(),
            "0204000000000000000000000094c60600",
        )

    async def test_connect_retries_transient_failures(self) -> None:
        ReconnectingClient.attempts = 0
        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=ReconnectingClient)

        await gatt.connect(retries=3, retry_delay=0)

        self.assertEqual(ReconnectingClient.attempts, 3)
        await gatt.close()

    async def test_find_proxy_device_matches_advertised_name(self) -> None:
        device = await find_proxy_device(FakeScanner, timeout=2.5)

        self.assertEqual(device.address, "BB")
        self.assertEqual(FakeScanner.filter_timeout, 2.5)

    async def test_find_proxy_device_matches_advertisement_local_name(self) -> None:
        # macOS: device.name can be None until cache warm; the match must use
        # the advertisement's local_name.
        device = await find_proxy_device(FakeScanner, timeout=1.0)

        class ColdDevice:
            name = None
            address = "CC"

        adv = type("Adv", (), {"local_name": "GFSK DFU PROXY"})()
        proxy_adv = type("Adv", (), {"local_name": "unrelated"})()

        self.assertEqual(device.address, "BB")
        # The proxy-name filter must accept a cold-cache device via adv data.
        class ColdDevice:
            name = None
            address = "CC"

        adv = type("Adv", (), {"local_name": "GFSK DFU PROXY"})()
        proxy_adv = type("Adv", (), {"local_name": "unrelated"})()

        async def match_only(match, timeout):
            return ColdDevice() if match(ColdDevice(), adv) else None

        async def mismatch_only(match, timeout):
            return ColdDevice() if match(ColdDevice(), proxy_adv) else None

        cold = await find_proxy_device(
            type("S", (), {"find_device_by_filter": staticmethod(match_only)}), timeout=1.0
        )
        self.assertEqual(cold.address, "CC")
        with self.assertRaises(SystemExit):
            await find_proxy_device(
                type("S", (), {"find_device_by_filter": staticmethod(mismatch_only)}), timeout=1.0
            )

    async def test_connect_acquires_linux_mtu_before_unacknowledged_writes(self) -> None:
        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=FakeClient)

        await gatt.connect()

        self.assertTrue(gatt.client._backend.mtu_acquired)
        await gatt.close()

    async def test_max_write_without_response_size_uses_characteristic_limit(self) -> None:
        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=FakeClient)
        await gatt.connect()

        self.assertEqual(gatt.max_write_without_response_size("data"), 128)
        await gatt.close()

    async def test_wait_for_write_without_response_size_allows_bluez_limit_to_settle(self) -> None:
        class SettlingCharacteristic:
            def __init__(self) -> None:
                self.sizes = iter((20, 192))

            @property
            def max_write_without_response_size(self) -> int:
                return next(self.sizes)

        characteristic = SettlingCharacteristic()

        class SettlingServices:
            @staticmethod
            def get_characteristic(uuid: str):
                return characteristic if uuid == "data" else None

        class SettlingClient(FakeClient):
            def __init__(self, device) -> None:
                super().__init__(device)
                self.services = SettlingServices()

        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=SettlingClient)
        await gatt.connect()

        self.assertEqual(await gatt.wait_for_write_without_response_size("data", 192, timeout=0.1, poll_interval=0), 192)

        await gatt.close()

    async def test_wait_for_write_without_response_size_uses_negotiated_mtu_when_bluez_property_sticks_at_20(self) -> None:
        class StuckCharacteristic:
            max_write_without_response_size = 20

        class StuckServices:
            @staticmethod
            def get_characteristic(uuid: str):
                return StuckCharacteristic() if uuid == "data" else None

        class StuckClient(FakeClient):
            mtu_size = 247

            def __init__(self, device) -> None:
                super().__init__(device)
                self.services = StuckServices()

        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=StuckClient)
        await gatt.connect()

        self.assertEqual(await gatt.wait_for_write_without_response_size("data", 192, timeout=0.1, poll_interval=0), 244)

        await gatt.close()

    async def test_max_write_without_response_size_requires_full_rf_frame(self) -> None:
        """A 20-byte unnegotiated MTU must fail before START erases target flash."""
        class SmallCharacteristic:
            max_write_without_response_size = 20

        class SmallServices:
            @staticmethod
            def get_characteristic(uuid: str):
                return SmallCharacteristic() if uuid == "data" else None

        class SmallClient(FakeClient):
            def __init__(self, device) -> None:
                super().__init__(device)
                self.services = SmallServices()

        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=SmallClient)
        await gatt.connect()

        with self.assertRaisesRegex(RuntimeError, "192 B"):
            gatt.require_write_without_response_size("data", 192)

        await gatt.close()

    async def test_gatt_reads_writes_and_queues_notifications(self) -> None:
        device = FakeDevice("GFSK DFU PROXY", "BB")
        gatt = BleakGatt(device, client_class=FakeClient)
        await gatt.connect()
        await gatt.start_notify("status")
        gatt.client.callbacks["status"]("status", bytearray(b"\x03"))

        self.assertEqual(await gatt.read("discovery"), b"\x01\x02")
        await gatt.write("command", b"\x04", response=True)
        self.assertEqual(await gatt.read_notification(timeout=0.1), b"\x03")
        self.assertEqual(gatt.client.writes, [("command", b"\x04", True)])

        await gatt.close()
        self.assertFalse(gatt.client.connected)

    async def test_notification_timeout_returns_none(self) -> None:
        gatt = BleakGatt(FakeDevice("GFSK DFU PROXY", "BB"), client_class=FakeClient)
        await gatt.connect()

        self.assertIsNone(await gatt.read_notification(timeout=0.001))
        await gatt.close()


if __name__ == "__main__":
    unittest.main()
