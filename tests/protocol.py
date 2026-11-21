"""Reference GDFU v1 binary codec and receiver session rules."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, IntEnum
import struct
import zlib

MAGIC = b"GDFU"
VERSION = 1
HEADER = struct.Struct(">4sBBBBIQIIH")
CRC = struct.Struct(">I")
MAX_PAYLOAD = 192


class FrameError(ValueError):
    pass


class Profile(IntEnum):
    MHZ_433 = 1
    MHZ_868 = 2


class PacketType(IntEnum):
    BEACON = 1
    START = 2
    READY = 3
    DATA = 4
    ACK = 5
    NACK = 6
    STATUS = 7
    FINISH = 8
    COMPLETE = 9
    ABORT = 10
    ERROR = 11
    SET_TX_POWER = 12


@dataclass(frozen=True)
class Frame:
    packet_type: PacketType
    profile: Profile
    target_id: int
    session_id: int
    sequence: int
    offset: int
    payload: bytes = b""


def encode_frame(frame: Frame) -> bytes:
    if not 0 <= frame.target_id <= 0xFFFFFFFF:
        raise FrameError("target ID out of range")
    if not 0 <= frame.session_id <= 0xFFFFFFFFFFFFFFFF:
        raise FrameError("session ID out of range")
    if not 0 <= frame.sequence <= 0xFFFFFFFF or not 0 <= frame.offset <= 0xFFFFFFFF:
        raise FrameError("sequence or offset out of range")
    if len(frame.payload) > MAX_PAYLOAD:
        raise FrameError("payload too large")
    raw = HEADER.pack(
        MAGIC,
        VERSION,
        int(frame.packet_type),
        int(frame.profile),
        0,
        frame.target_id,
        frame.session_id,
        frame.sequence,
        frame.offset,
        len(frame.payload),
    ) + frame.payload
    return raw + CRC.pack(zlib.crc32(raw) & 0xFFFFFFFF)


def decode_frame(raw: bytes) -> Frame:
    if len(raw) < HEADER.size + CRC.size:
        raise FrameError("frame too short")
    header = raw[:HEADER.size]
    magic, version, packet_type, profile, flags, target_id, session_id, sequence, offset, length = HEADER.unpack(header)
    if magic != MAGIC:
        raise FrameError("bad magic")
    if version != VERSION:
        raise FrameError("unsupported version")
    if flags:
        raise FrameError("unsupported flags")
    if length > MAX_PAYLOAD or len(raw) != HEADER.size + length + CRC.size:
        raise FrameError("invalid length")
    expected_crc = CRC.unpack(raw[-CRC.size:])[0]
    actual_crc = zlib.crc32(raw[:-CRC.size]) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise FrameError("CRC mismatch")
    try:
        return Frame(PacketType(packet_type), Profile(profile), target_id, session_id, sequence, offset, raw[HEADER.size:-CRC.size])
    except ValueError as error:
        raise FrameError("unsupported packet type or profile") from error


class TransferPhase(Enum):
    ADVERTISING = "advertising"
    RECEIVING = "receiving"
    FAILED = "failed"


@dataclass(frozen=True)
class TransferEvent:
    kind: str
    session_id: int
    sequence: int = 0
    offset: int = 0
    length: int = 0

    @classmethod
    def accept(cls, session_id: int) -> "TransferEvent":
        return cls("accept", session_id)

    @classmethod
    def data(cls, session_id: int, sequence: int, offset: int, length: int) -> "TransferEvent":
        return cls("data", session_id, sequence, offset, length)


@dataclass(frozen=True)
class ReceiveResult:
    confirmed_offset: int
    duplicate: bool


class TransferState:
    def __init__(self, target_id: int, profile: Profile) -> None:
        self.target_id = target_id
        self.profile = profile
        self.phase = TransferPhase.ADVERTISING
        self.session_id: int | None = None
        self.confirmed_offset = 0
        self._last_sequence: int | None = None
        self._last_offset: int | None = None

    def apply(self, event: TransferEvent) -> ReceiveResult:
        if event.kind == "accept":
            if self.phase is not TransferPhase.ADVERTISING or event.session_id == 0:
                raise FrameError("invalid session acceptance")
            self.phase = TransferPhase.RECEIVING
            self.session_id = event.session_id
            return ReceiveResult(self.confirmed_offset, False)
        if event.kind != "data" or self.phase is not TransferPhase.RECEIVING:
            raise FrameError("invalid transfer event")
        if event.session_id != self.session_id:
            raise FrameError("wrong session")
        if not 0 < event.length <= MAX_PAYLOAD:
            raise FrameError("invalid data length")
        if event.sequence == self._last_sequence and event.offset == self._last_offset:
            return ReceiveResult(self.confirmed_offset, True)
        if event.offset != self.confirmed_offset:
            raise FrameError("unexpected offset")
        self.confirmed_offset += event.length
        self._last_sequence = event.sequence
        self._last_offset = event.offset
        return ReceiveResult(self.confirmed_offset, False)
