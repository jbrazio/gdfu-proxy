"""ESP32 BLE-to-RF proxy bridge model with bounded queue and target discovery."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, IntEnum
import time


class ProxyState(Enum):
    IDLE = "idle"
    STREAMING = "streaming"
    BUSY = "busy"
    ERROR = "error"


class ProxyCommand(IntEnum):
    SELECT_TARGET = 1
    START_DFU = 2
    ABORT = 3
    RESET_RADIO = 4
    FINISH_DFU = 5


class ProxyError(ValueError):
    pass


@dataclass(frozen=True)
class RadioConfig:
    profile: int = 1
    frequency_hz: int = 433175000
    tx_power_dbm: int = 10
    bitrate_bps: int = 80000
    fdev_hz: int = 28000
    rx_bw_hz: int = 187200


@dataclass
class DiscoveredTarget:
    target_id: int
    profile: int
    rssi: int
    last_seen: float


class ProxyBridge:
    def __init__(self, ttl_seconds: float = 15.0, queue_capacity: int = 8) -> None:
        self.ttl_seconds = ttl_seconds
        self.queue_capacity = queue_capacity
        self.state = ProxyState.IDLE
        self.selected_target_id: int | None = None
        self.active_session_id: int | None = None
        self._targets: dict[int, DiscoveredTarget] = {}
        self._queue: list[bytes] = []
        self.accepted_bytes = 0
        self.confirmed_bytes = 0

    def handle_beacon(self, target_id: int, profile: int, rssi: int, timestamp: float | None = None) -> None:
        ts = timestamp if timestamp is not None else time.time()
        self._targets[target_id] = DiscoveredTarget(target_id, profile, rssi, ts)

    def get_discovered_targets(self, now: float | None = None) -> list[DiscoveredTarget]:
        current_time = now if now is not None else time.time()
        active = [
            t for t in self._targets.values()
            if (current_time - t.last_seen) <= self.ttl_seconds
        ]
        return active

    def select_target(self, target_id: int) -> None:
        self.selected_target_id = target_id

    def start_session(self, session_id: int) -> None:
        if self.selected_target_id is None:
            raise ProxyError("no target selected")
        self.active_session_id = session_id
        self.state = ProxyState.STREAMING
        self._queue.clear()
        self.accepted_bytes = 0
        self.confirmed_bytes = 0

    def write_data(self, chunk: bytes) -> bool:
        if self.state not in (ProxyState.STREAMING, ProxyState.BUSY):
            return False
        if len(self._queue) >= self.queue_capacity:
            self.state = ProxyState.BUSY
            return False

        self._queue.append(chunk)
        self.accepted_bytes += len(chunk)
        if len(self._queue) >= self.queue_capacity:
            self.state = ProxyState.BUSY
        else:
            self.state = ProxyState.STREAMING
        return True

    def confirm_rf_ack(self, length: int) -> None:
        if self._queue:
            self._queue.pop(0)
        self.confirmed_bytes += length
        if len(self._queue) < self.queue_capacity:
            self.state = ProxyState.STREAMING

    def handle_ble_disconnect(self) -> None:
        self.state = ProxyState.IDLE
        self.active_session_id = None
        self.selected_target_id = None
        self._queue.clear()
