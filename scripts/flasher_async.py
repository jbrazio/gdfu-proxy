#!/usr/bin/env python3
"""Cross-platform BLE-to-GFSK firmware streamer."""

import asyncio
import struct
import sys
import time
import zipfile

from gatt_protocol import (
    COMMAND_UUID,
    DATA_UUID,
    DISCOVERY_UUID,
    STATUS_UUID,
    application_chunks,
    application_transfer_bytes,
    select_live_target,
)

PROXY_NAME = "GFSK DFU PROXY"
CHUNK_SIZE = 192
MAX_QUEUE_CHUNKS = 8


def crc16_nordic(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = ((crc >> 8) & 0xFF) | ((crc << 8) & 0xFFFF)
        crc ^= byte
        crc ^= (crc & 0xFF) >> 4
        crc ^= ((crc << 8) << 4) & 0xFFFF
        crc ^= (((crc & 0xFF) << 4) << 1) & 0xFFFF
    return crc & 0xFFFF


async def find_proxy_device(scanner, name: str = PROXY_NAME, timeout: float = 12.0):
    """Return first BLE device advertising the configured proxy name.

    Uses find_device_by_filter so scanning stops at the first match and
    advertisement local_name is honoured (macOS caches device.name=None)."""
    device = await scanner.find_device_by_filter(
        lambda d, ad: (ad.local_name or d.name or "").lower() == name.lower(),
        timeout=timeout,
    )
    if device is None:
        raise SystemExit(f"ERROR: no BLE device advertising '{name}' found in {timeout:.0f}s")
    return device


class BleakGatt:
    def __init__(self, device, client_class) -> None:
        self.client = client_class(device)
        self.notifications: asyncio.Queue[bytes] = asyncio.Queue()

    async def connect(self, retries: int = 3, retry_delay: float = 1.5) -> None:
        for attempt in range(retries):
            try:
                await self.client.connect()
                backend = getattr(self.client, "_backend", self.client)
                acquire_mtu = getattr(backend, "_acquire_mtu", None)
                if acquire_mtu is not None:
                    await acquire_mtu()
                return
            except Exception:
                if attempt + 1 == retries:
                    raise
                await asyncio.sleep(retry_delay)

    async def read(self, uuid: str) -> bytes:
        return bytes(await self.client.read_gatt_char(uuid))

    async def write(self, uuid: str, data: bytes, *, response: bool) -> None:
        await self.client.write_gatt_char(uuid, data, response=response)

    def max_write_without_response_size(self, uuid: str) -> int:
        characteristic = self.client.services.get_characteristic(uuid)
        if characteristic is None:
            raise RuntimeError(f"missing GATT characteristic {uuid}")
        return characteristic.max_write_without_response_size

    def require_write_without_response_size(self, uuid: str, minimum: int) -> int:
        size = self.max_write_without_response_size(uuid)
        if size < minimum:
            raise RuntimeError(f"BLE payload limit too small ({size} B; need {minimum} B)")
        return size

    async def wait_for_write_without_response_size(
        self, uuid: str, minimum: int, timeout: float = 10.0, poll_interval: float = 0.5
    ) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            size = max(self.max_write_without_response_size(uuid), self.client.mtu_size - 3)
            if size >= minimum:
                return size
            await asyncio.sleep(poll_interval)
        return self.require_write_without_response_size(uuid, minimum)

    async def start_notify(self, uuid: str) -> None:
        def received(_, data: bytearray) -> None:
            self.notifications.put_nowait(bytes(data))

        await self.client.start_notify(uuid, received)

    async def read_notification(self, timeout: float) -> bytes | None:
        try:
            return await asyncio.wait_for(self.notifications.get(), timeout)
        except TimeoutError:
            return None

    async def close(self) -> None:
        if self.client.is_connected:
            await self.client.disconnect()


def start_dfu_command(total_bytes: int) -> bytes:
    return b"\x02" + struct.pack("<B3xIII", 0x04, 0, 0, total_bytes)


def parse_status(raw: bytes):
    if len(raw) < 10:
        return None
    return raw[0], struct.unpack(">I", raw[1:5])[0], struct.unpack(">I", raw[5:9])[0], raw[9]


async def read_status(gatt: BleakGatt):
    return parse_status(await gatt.read(STATUS_UUID))


async def wait_proxy_idle(gatt: BleakGatt, timeout: float = 45.0) -> bool:
    """Wait until the proxy leaves a busy/errored session.

    Notifications (subscribed at connect) drive the wait; a 1 s GATT read is
    only the fallback when no notification arrives, so IDLE is observed in
    ~100 ms instead of a full sleep tick."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = await gatt.read_notification(1.0)
        if raw is None:
            raw = await gatt.read(STATUS_UUID)
        status = parse_status(raw)
        if status and status[0] == 1 and status[3] == 0:
            return True
        if status and status[0] == 4:
            await gatt.write(COMMAND_UUID, b"\x03", response=True)
    await gatt.write(COMMAND_UUID, b"\x04", response=True)
    await asyncio.sleep(2.0)
    status = await read_status(gatt)
    return bool(status and status[0] == 1)


async def main() -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise SystemExit("ERROR: install updater dependency with: python3 -m pip install -r requirements.txt") from error

    package_path = sys.argv[1] if len(sys.argv) > 1 else "firmware/Heltec_t114_repeater.zip"
    limit_chunks = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    with zipfile.ZipFile(package_path) as package:
        init_data = package.read("firmware.dat")
        image = package.read("firmware.bin")

    app_size = len(image)
    total_bytes = application_transfer_bytes(app_size, limit_chunks)
    print(f"=== Opening firmware package: {package_path} ===")
    print(f"Firmware image size: {app_size} bytes ({app_size / 1024:.1f} KB)")
    print(f"Computed image CRC16: 0x{crc16_nordic(image):04X}")

    device = await find_proxy_device(BleakScanner)
    print(f"\n[1] Connecting to proxy {PROXY_NAME} at {device.address}...")
    gatt = BleakGatt(device, BleakClient)
    try:
        await gatt.connect()
        await gatt.start_notify(STATUS_UUID)
        print("Connected. STATUS notifications enabled.")

        await gatt.write(COMMAND_UUID, b"\x03", response=True)
        if not await wait_proxy_idle(gatt):
            raise SystemExit("ERROR: proxy did not return to IDLE")

        print("[2] Discovering target...")
        target = None
        for _ in range(12):
            target = select_live_target(await gatt.read(DISCOVERY_UUID))
            if target:
                break
            await asyncio.sleep(0.5)
        if target is None:
            raise SystemExit("ERROR: target discovery failed")
        print(f"Target found: ID=0x{target.hex()}")
        selected = False
        for _ in range(12):
            await gatt.write(COMMAND_UUID, b"\x01" + target, response=True)
            for _ in range(3):
                status = parse_status(await gatt.read_notification(1.0) or b"")
                if status and status[0] in (1, 3):
                    selected = True
                    break
                if status and status[0] == 4:
                    print(f"SELECT rejected (error {status[3]}), retrying...")
                    break
            if selected:
                break
            await asyncio.sleep(0.5)
        if not selected:
            raise SystemExit("ERROR: target not selectable (beacon TTL may have expired)")

        print(f"[3] Sending START_DFU (app_size={total_bytes} B)...")
        await gatt.write(COMMAND_UUID, start_dfu_command(total_bytes), response=True)
        streaming = False
        for _ in range(25):
            status = parse_status(await gatt.read_notification(1.0) or b"")
            if status and status[0] == 3:
                streaming = True
                break
            if status and status[0] == 4:
                raise SystemExit(f"ERROR: proxy error {status[3]} on START")
        if not streaming:
            raise SystemExit("ERROR: target did not enter STREAMING state")

        print(f"[4] Sending INIT packet ({len(init_data)} B)...")
        await gatt.write(DATA_UUID, init_data, response=True)
        init_acked = False
        for _ in range(5):
            status = parse_status(await gatt.read_notification(1.0) or b"")
            if status and status[2] >= len(init_data):
                init_acked = True
                break
        if not init_acked:
            raise SystemExit("ERROR: INIT packet not acknowledged")

        # One proxy RF frame carries one BLE write. Refuse a default 20-byte
        # ATT MTU before START can erase the target's only application bank.
        chunk_size = min(CHUNK_SIZE, await gatt.wait_for_write_without_response_size(DATA_UUID, CHUNK_SIZE))
        chunks = application_chunks(image[:total_bytes], chunk_size)
        print(f"[5] Notification-driven streaming: {len(chunks)} chunks ({total_bytes} B)...")
        started = time.monotonic()
        sent_chunks = confirmed_bytes = last_print = 0
        while sent_chunks < len(chunks) and sent_chunks < MAX_QUEUE_CHUNKS:
            await gatt.write(DATA_UUID, chunks[sent_chunks], response=False)
            sent_chunks += 1
        while confirmed_bytes < total_bytes:
            status = parse_status(await gatt.read_notification(3.0) or await gatt.read(STATUS_UUID))
            if not status:
                continue
            if status[0] == 4:
                raise SystemExit(f"ERROR: proxy error code {status[3]}")
            confirmed_bytes = max(0, status[2] - len(init_data))
            confirmed_chunks = confirmed_bytes // chunk_size
            if confirmed_chunks >= last_print + 25 or confirmed_bytes >= total_bytes:
                last_print = (confirmed_chunks // 25) * 25
                elapsed = time.monotonic() - started
                print(f"  Chunk {confirmed_chunks}/{len(chunks)}: {confirmed_bytes}/{total_bytes} B [{elapsed:.0f}s elapsed]")
            while sent_chunks < len(chunks) and sent_chunks * chunk_size - confirmed_bytes < MAX_QUEUE_CHUNKS * chunk_size:
                await gatt.write(DATA_UUID, chunks[sent_chunks], response=False)
                sent_chunks += 1

        if limit_chunks == 0 or total_bytes == app_size:
            print("[6] Sending FINISH_DFU command...")
            await gatt.write(COMMAND_UUID, b"\x05", response=True)
            # Postvalidation (CRC16 + bank activation) runs on the target and
            # can still fail for up to ~10 s; confirm IDLE before declaring success.
            for _ in range(15):
                status = parse_status(await gatt.read_notification(1.0) or b"")
                if status and status[0] == 1 and status[3] == 0:
                    print("=== FIRMWARE FLASH PROCESS COMPLETED SUCCESSFULLY! ===")
                    break
                if status and status[0] == 4:
                    raise SystemExit(f"ERROR: postvalidation failed (proxy error {status[3]})")
            else:
                raise SystemExit("ERROR: FINISH not acknowledged within 15 s")
        else:
            await gatt.write(COMMAND_UUID, b"\x03", response=True)
            print(f"[Test mode] {total_bytes} bytes flashed and verified on RF.")
    finally:
        await gatt.close()


if __name__ == "__main__":
    asyncio.run(main())
