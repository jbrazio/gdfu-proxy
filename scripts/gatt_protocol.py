"""GATT discovery helpers shared by RF flashers."""

import re

SERVICE_UUID = "7e4c2a10-8b31-4f9d-a600-000000000000"
DISCOVERY_UUID = "7e4c2a10-8b31-4f9d-a600-000000000020"
COMMAND_UUID = "7e4c2a10-8b31-4f9d-a600-000000000030"
DATA_UUID = "7e4c2a10-8b31-4f9d-a600-000000000031"
STATUS_UUID = "7e4c2a10-8b31-4f9d-a600-000000000032"
CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"
REQUIRED_UUIDS = (DISCOVERY_UUID, COMMAND_UUID, DATA_UUID, STATUS_UUID)


class GattContractError(RuntimeError):
    pass


def _normalize_uuid(value: str) -> str:
    value = value.lower()
    if len(value) == 4:
        return f"0000{value}-0000-1000-8000-00805f9b34fb"
    return value


def value_handles(listing: str) -> dict[str, str]:
    """Return current value handles for required proxy characteristics."""
    handles: dict[str, str] = {}
    for handle, uuid in re.findall(
        r"(?:char\s+)?value\s+handle\s*[:=]\s*(0x[0-9a-f]+).*?uuid\s*[:=]\s*([0-9a-f-]+)",
        listing,
        re.IGNORECASE,
    ):
        handles[_normalize_uuid(uuid)] = handle.lower()
    missing = [uuid for uuid in REQUIRED_UUIDS if uuid not in handles]
    if missing:
        raise GattContractError(f"proxy missing required characteristic(s): {', '.join(missing)}")
    return {uuid: handles[uuid] for uuid in REQUIRED_UUIDS}


def status_cccd_handle(handles: dict[str, str], descriptors: str) -> str:
    """Find STATUS's CCCD between STATUS and next characteristic value handle."""
    status = int(handles[STATUS_UUID], 16)
    next_values = sorted(int(value, 16) for value in handles.values() if int(value, 16) > status)
    upper = next_values[0] if next_values else 0x10000
    for handle, uuid in re.findall(
        r"handle\s*[:=]\s*(0x[0-9a-f]+).*?uuid\s*[:=]\s*([0-9a-f-]+)",
        descriptors,
        re.IGNORECASE,
    ):
        value = int(handle, 16)
        if status < value < upper and _normalize_uuid(uuid) == CCCD_UUID:
            return handle.lower()
    raise GattContractError("proxy STATUS characteristic has no CCCD")


def controller_address(adapter: str, listing: str) -> str:
    """Return adapter address from `hciconfig -a` output."""
    match = re.search(
        rf"\b{re.escape(adapter)}:\s+Type:.*?\n\s*BD Address:\s*([0-9A-Fa-f:]{{17}})",
        listing,
        re.DOTALL,
    )
    if not match:
        raise GattContractError(f"Bluetooth adapter {adapter} not found")
    return match.group(1).upper()


def bluetoothctl_scan_commands(adapter_address: str) -> str:
    """Interactive bluetoothctl input pinned to one controller."""
    return f"select {adapter_address}\nscan on\n"


def proxy_address(output: str, name: str) -> str | None:
    """Return MAC from `[NEW|CHG] Device <MAC> <name>` / `Device <MAC> <name>` lines only."""
    clean = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", output).replace("\r", "\n")
    for match in re.finditer(
        r"(?:^|\s)Device\s+((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})\s+(.*)$", clean, re.MULTILINE
    ):
        if match.group(2).strip().lower() == name.lower():
            return match.group(1).upper()
    return None


def application_transfer_bytes(app_size: int, limit_chunks: int = 0, chunk_size: int = 192) -> int:
    """Return RF DATA bytes; only a complete application may need final padding."""
    if app_size < 0 or limit_chunks < 0 or chunk_size <= 0:
        raise ValueError("invalid transfer size")
    requested = min(app_size, limit_chunks * chunk_size) if limit_chunks else app_size
    return (requested + 3) & ~3


def application_chunks(image: bytes, chunk_size: int = 192) -> tuple[bytes, ...]:
    """Split application DATA; pad final partial flash word with FF."""
    if not image or chunk_size <= 0:
        return ()
    chunks = tuple(image[index : index + chunk_size] for index in range(0, len(image), chunk_size))
    last = chunks[-1]
    if len(last) % 4:
        last += b"\xff" * (4 - len(last) % 4)
        chunks = (*chunks[:-1], last)
    return chunks


def select_live_target(discovery: bytes) -> bytes | None:
    """Return ID for fresh target with longest advertised TTL."""
    if not discovery:
        return None
    best: tuple[int, bytes] | None = None
    for index in range(min(discovery[0], 16)):
        base = 1 + index * 8
        if base + 8 > len(discovery):
            break
        ttl = int.from_bytes(discovery[base + 6 : base + 8], "big")
        if ttl and (best is None or ttl > best[0]):
            best = (ttl, discovery[base : base + 4])
    return best[1] if best else None
