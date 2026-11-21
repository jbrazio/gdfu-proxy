#!/usr/bin/env python3
"""Hardware test for the proxy FILE MANAGEMENT GATT group (docs/PROTOCOL_BLE.md).

Builds a mockup firmware package (mockup.zip), uploads it to the proxy and
exercises every file operation: storage info, list, upload, download, replace,
delete, and each error path.

    python3 tests/hardware/test_file_management.py            # find the proxy by name
    python3 tests/hardware/test_file_management.py --mac 10:51:DB:2B:06:91
    python3 tests/hardware/test_file_management.py --sim      # fake proxy: checks the harness only

Requires the proxy flashed with the file-management firmware (full USB flash:
the partition table changed). `--sim` proves nothing about the firmware.
"""

import argparse
import io
import json
import os
import re
import signal
import struct
import subprocess
import sys
import time
import unittest
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from gatttool_link import AsyncGatt, crc16_nordic, find_proxy_mac  # noqa: E402

UUID_BASE = "7e4c2a10-8b31-4f9d-a600-0000000000"
CMD, DATA, STATUS, STORAGE = (UUID_BASE + s for s in ("40", "41", "42", "43"))
MAX_CHUNK = 160
MTU = 247

OK, E_COMMAND, E_BUSY, E_NAME, E_NO_SPACE, E_EXISTS, E_NOT_FOUND = 0, 1, 2, 3, 4, 5, 6
E_IO, E_OFFSET, E_CRC, E_INCOMPLETE, E_NO_UPLOAD, E_PACKAGE = 7, 8, 9, 10, 11, 12

ARGS = argparse.Namespace(sim=False, mac=None, size=3000)


def build_mockup_zip(app_size: int) -> bytes:
    """A package the app's parseFirmwareZip accepts (device type 82, valid CRC16)."""
    app = bytes((i * 31 + 7) & 0xFF for i in range(app_size))
    manifest = {
        "manifest": {
            "application": {
                "bin_file": "firmware.bin",
                "dat_file": "firmware.dat",
                "init_packet_data": {
                    "application_version": 1,
                    "device_revision": 65535,  # supported profile value
                    "device_type": 82,
                    "firmware_crc16": crc16_nordic(app),
                    "softdevice_req": [254],
                },
            },
            "dfu_version": 0.5,
        }
    }
    out = io.BytesIO()
    # Stored entries, matching the real release packages: the proxy validator
    # (and the app, after Task 4 parity) accepts method 0 only.
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("manifest.json", json.dumps(manifest))
        z.writestr("firmware.dat", bytes(range(14)))
        z.writestr("firmware.bin", app)
    return out.getvalue()


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ---- links: a real proxy over gatttool, or a fake one ----------------------


class GatttoolLink:
    def __init__(self, mac: str):
        self.mac = mac
        self.gatt = None
        self.handles: dict[str, str] = {}
        self.connect()

    @staticmethod
    def _kill_stale() -> None:
        """gatttool children survive a killed parent and hold the proxy's only connection."""
        subprocess.run(["pkill", "-x", "gatttool"], capture_output=True)
        time.sleep(1.0)

    def connect(self) -> None:
        last = ""
        for attempt in range(3):
            self._kill_stale()
            self.gatt = AsyncGatt(self.mac)
            last = self.gatt.send_and_wait("connect", "Connection successful", timeout=12.0)
            if "Connection successful" in last:
                break
            self._close_gatt()
            time.sleep(1.0 + attempt)
        else:
            raise RuntimeError(f"cannot connect to {self.mac}: {last.strip()}")
        self.gatt.send_and_wait(f"mtu {MTU}", "MTU was exchanged successfully", timeout=6.0)
        listing = self.gatt.send_and_wait("characteristics", STORAGE, timeout=8.0)
        for value_handle, uuid in re.findall(r"char value handle: (0x[0-9a-f]+), uuid: (\S+)", listing):
            self.handles[uuid] = value_handle
        if STORAGE not in self.handles:
            raise RuntimeError("proxy has no file-management characteristics; flash the new firmware")

    def read(self, uuid: str) -> bytes:
        return self.gatt.read_handle(self.handles[uuid])

    def write(self, uuid: str, data: bytes) -> None:
        if not self.gatt.write_req(self.handles[uuid], data.hex()):
            raise RuntimeError(f"write to {uuid} not acknowledged")

    def reconnect(self) -> None:
        """Force a radio-level disconnect: gatttool quit alone keeps the link."""
        if self.gatt:
            subprocess.run(
                ["bluetoothctl", "disconnect", self.mac], capture_output=True, timeout=15
            )
            time.sleep(2.0)
        self.close()
        self._kill_stale()
        self.connect()

    def _close_gatt(self) -> None:
        if not self.gatt:
            return
        try:
            self.gatt.send_line("quit")
        except OSError:
            pass
        time.sleep(0.3)
        try:
            os.close(self.gatt.master)
        except OSError:
            pass
        for _ in range(20):
            try:
                pid, _ = os.waitpid(self.gatt.pid, os.WNOHANG)
            except ChildProcessError:
                break
            if pid:
                break
            time.sleep(0.1)
        else:
            try:
                os.kill(self.gatt.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.gatt = None

    def close(self) -> None:
        self._close_gatt()


class SimLink:
    """Mirrors proxy/src/main.cpp + gdfu_file.c semantics, for checking the harness."""

    TOTAL = 0x4C0000

    def __init__(self):
        self.files: dict[str, bytes] = {}
        self.reconnect()

    def reconnect(self) -> None:
        self.up = None
        self.dl = None
        self.err = 0
        self.data = b""

    def close(self) -> None:
        pass

    def _used(self) -> int:
        return sum(-(-len(v) // 4096) * 4096 for v in self.files.values())

    @staticmethod
    def _valid(name: str) -> bool:
        return (
            5 <= len(name) <= 63
            and name[0] != "."
            and re.fullmatch(r"[A-Za-z0-9._-]+", name) is not None
            and name.lower().endswith(".zip")
        )

    def _fits(self, size: int) -> bool:
        need = -(-size // 4096) * 4096 + 3 * 4096
        return size > 0 and need <= self.TOTAL - self._used()

    def _fail(self, e: int) -> None:
        self.err = self.err or e

    def _is_package(self, buf: bytes) -> bool:
        """Mirror the proxy's stored-package validation (docs/PROTOCOL_BLE.md):
        ZIP magic, required stored entries, no extras or duplicates."""
        if buf[:4] != b"PK\x03\x04":
            return False
        try:
            names = []
            with zipfile.ZipFile(io.BytesIO(buf)) as z:
                for info in z.infolist():
                    if info.compress_type != zipfile.ZIP_STORED:
                        return False
                    names.append(info.filename)
        except (zipfile.BadZipFile, OSError):
            return False
        required = {"manifest.json", "firmware.dat", "firmware.bin"}
        return sorted(names) == sorted(required)

    def read(self, uuid: str) -> bytes:
        if uuid == STORAGE:
            return struct.pack(">IIH", self.TOTAL, self._used(), len(self.files))
        if uuid == STATUS:
            op = 1 if self.up else 2 if self.dl is not None else 0
            done = len(self.up["buf"]) if self.up else 0
            total = self.up["size"] if self.up else len(self.dl or b"")
            return struct.pack(">BBII", op, self.err, done, total)
        return self.data

    def write(self, uuid: str, p: bytes) -> None:
        if uuid == DATA:
            if len(p) <= 4:
                self._fail(E_COMMAND)
            elif not self.up:
                self._fail(E_NO_UPLOAD)
            elif len(p) - 4 > MAX_CHUNK:
                self._fail(E_COMMAND)
            elif struct.unpack(">I", p[:4])[0] != len(self.up["buf"]) or len(p) - 4 > self.up["size"] - len(self.up["buf"]):
                self._fail(E_OFFSET)
            else:
                self.up["buf"] += p[4:]
            return
        if p[0] != 3:
            self.err = 0
        c = p[0]

        def name(at: int) -> str | None:
            if len(p) != at + 1 + p[at] if len(p) > at else True:
                return None
            return p[at + 1 : at + 1 + p[at]].decode(errors="replace")

        if c == 1:
            i = struct.unpack(">H", p[1:3])[0]
            items = list(self.files.items())
            self.data = b"\x00" if i >= len(items) else b"\x01" + struct.pack(">I", len(items[i][1])) + items[i][0].encode()
        elif c == 2:
            size, crc, replace = struct.unpack(">IIB", p[1:10])
            n = name(10)
            if n is None:
                self._fail(E_COMMAND)
            elif not self._valid(n):
                self._fail(E_NAME)
            elif self.up or self.dl is not None:
                self._fail(E_BUSY)
            elif not self._fits(size):
                self._fail(E_NO_SPACE)
            elif n in self.files and not replace:
                self._fail(E_EXISTS)
            else:
                self.up = {"name": n, "size": size, "crc": crc, "buf": b""}
        elif c == 3:
            u = self.up
            if not u:
                self._fail(E_NO_UPLOAD)
            else:
                if len(u["buf"]) != u["size"]:
                    self._fail(E_INCOMPLETE)
                elif crc32(u["buf"]) != u["crc"]:
                    self._fail(E_CRC)
                elif not self._is_package(u["buf"]):
                    self._fail(E_PACKAGE)
                else:
                    self.files[u["name"]] = u["buf"]
                self.up = None
        elif c == 4:
            self.up = None
        elif c == 5:
            n = name(1)
            if n is None or not self._valid(n):
                self._fail(E_NAME)
            elif self.up or self.dl is not None:
                self._fail(E_BUSY)
            elif n not in self.files:
                self._fail(E_NOT_FOUND)
            else:
                del self.files[n]
        elif c == 6:
            n = name(1)
            if n is None or not self._valid(n):
                self._fail(E_NAME)
            elif n not in self.files:
                self._fail(E_NOT_FOUND)
            else:
                self.dl = self.files[n]
        elif c == 7:
            off, ln = struct.unpack(">IB", p[1:6])
            if self.dl is None:
                self._fail(E_NOT_FOUND)
            else:
                self.data = self.dl[off : off + ln]
        elif c == 8:
            self.dl = None
        else:
            self._fail(E_COMMAND)


# ---- file operations -------------------------------------------------------


class ProxyError(Exception):
    def __init__(self, code: int):
        super().__init__(f"proxy file error {code}")
        self.code = code


class FileOps:
    def __init__(self, link):
        self.link = link

    def status(self) -> dict:
        op, err, done, total = struct.unpack(">BBII", self.link.read(STATUS)[:10])
        return {"op": op, "err": err, "done": done, "total": total}

    def command(self, cmd: int, payload: bytes = b"") -> None:
        self.link.write(CMD, bytes([cmd]) + payload)
        err = self.status()["err"]
        if err:
            raise ProxyError(err)

    @staticmethod
    def name_field(name: str) -> bytes:
        raw = name.encode()
        return bytes([len(raw)]) + raw

    def storage(self) -> dict:
        total, used, count = struct.unpack(">IIH", self.link.read(STORAGE)[:10])
        return {"total": total, "used": used, "count": count}

    def list(self) -> dict[str, int]:
        files = {}
        for i in range(1000):
            self.command(1, struct.pack(">H", i))
            raw = self.link.read(DATA)
            if not raw or raw[0] == 0:
                return files
            files[raw[5:].decode()] = struct.unpack(">I", raw[1:5])[0]
        raise AssertionError("list did not terminate")

    def upload_begin(self, name: str, size: int, crc: int, replace: bool = False) -> None:
        self.command(2, struct.pack(">IIB", size, crc, int(replace)) + self.name_field(name))

    def chunk(self, offset: int, data: bytes) -> None:
        self.link.write(DATA, struct.pack(">I", offset) + data)

    def upload(self, name: str, data: bytes, replace: bool = False) -> None:
        self.upload_begin(name, len(data), crc32(data), replace)
        try:
            for off in range(0, len(data), MAX_CHUNK):
                self.chunk(off, data[off : off + MAX_CHUNK])
            self.command(3)
        except Exception:
            self.link.write(CMD, bytes([4]))
            raise

    def download(self, name: str) -> bytes:
        self.command(6, self.name_field(name))
        try:
            total = self.status()["total"]
            out = b""
            while len(out) < total:
                want = min(MAX_CHUNK, total - len(out))
                self.command(7, struct.pack(">IB", len(out), want))
                block = self.link.read(DATA)
                assert len(block) == want, f"short block {len(block)} != {want}"
                out += block
            return out
        finally:
            self.link.write(CMD, bytes([8]))

    def delete(self, name: str) -> None:
        self.command(5, self.name_field(name))


# ---- tests -----------------------------------------------------------------

NAME = "mockup.zip"
link = None


def setUpModule():
    global link
    if ARGS.sim:
        link = SimLink()
    else:
        link = GatttoolLink(ARGS.mac or find_proxy_mac())


def tearDownModule():
    if link:
        link.close()


class FileManagementTests(unittest.TestCase):
    def setUp(self):
        self.ops = FileOps(link)
        self.mockup = build_mockup_zip(ARGS.size)
        link.write(CMD, bytes([4]))  # abort any leftover upload
        link.write(CMD, bytes([8]))  # close any leftover download
        for name in list(self.ops.list()):
            self.ops.delete(name)

    def tearDown(self):
        try:
            link.write(CMD, bytes([4]))
            link.write(CMD, bytes([8]))
            for name in list(self.ops.list()):
                self.ops.delete(name)
        except Exception:
            pass

    def assertProxyError(self, code: int, fn, *args, **kwargs):
        with self.assertRaises(ProxyError) as ctx:
            fn(*args, **kwargs)
        self.assertEqual(ctx.exception.code, code)

    def test_storage_info(self):
        s = self.ops.storage()
        self.assertGreater(s["total"], 4 * 1024 * 1024)  # 4.75 MiB partition
        self.assertLessEqual(s["used"], s["total"])
        self.assertEqual(s["count"], 0)

    def test_upload_list_download_roundtrip(self):
        before = self.ops.storage()
        self.ops.upload(NAME, self.mockup)
        self.assertEqual(self.ops.list(), {NAME: len(self.mockup)})
        after = self.ops.storage()
        self.assertEqual(after["count"], 1)
        self.assertGreater(after["used"], before["used"])
        self.assertEqual(self.ops.download(NAME), self.mockup)
        self.assertEqual(self.ops.status()["op"], 0)

    def test_duplicate_requires_replace(self):
        self.ops.upload(NAME, self.mockup)
        self.assertProxyError(E_EXISTS, self.ops.upload_begin, NAME, 10, 0, False)
        self.assertEqual(self.ops.status()["op"], 0)  # refused before opening a transfer
        smaller = build_mockup_zip(max(64, ARGS.size // 2))
        self.ops.upload(NAME, smaller, replace=True)
        self.assertEqual(self.ops.list(), {NAME: len(smaller)})
        self.assertEqual(self.ops.download(NAME), smaller)

    def test_delete(self):
        self.ops.upload(NAME, self.mockup)
        used = self.ops.storage()["used"]
        self.ops.delete(NAME)
        self.assertEqual(self.ops.list(), {})
        self.assertLess(self.ops.storage()["used"], used)
        self.assertProxyError(E_NOT_FOUND, self.ops.delete, NAME)
        self.assertProxyError(E_NOT_FOUND, self.ops.download, NAME)

    def test_invalid_names_rejected(self):
        for bad in ("mock.bin", ".zip", ".hidden.zip", "a/b.zip", "../x.zip", "a b.zip", "x" * 60 + ".zip", "möck.zip"):
            with self.subTest(name=bad):
                self.assertProxyError(E_NAME, self.ops.upload_begin, bad, 100, 0)
                self.assertProxyError(E_NAME, self.ops.delete, bad)
        self.assertEqual(self.ops.status()["op"], 0)

    def test_no_space_refused_before_transfer(self):
        total = self.ops.storage()["total"]
        self.assertProxyError(E_NO_SPACE, self.ops.upload_begin, NAME, total, 0)
        self.assertProxyError(E_NO_SPACE, self.ops.upload_begin, NAME, 0x7FFFFFFF, 0)
        self.assertEqual(self.ops.status()["op"], 0)

    def test_out_of_order_chunk(self):
        data = self.mockup
        self.ops.upload_begin(NAME, len(data), crc32(data))
        self.ops.chunk(0, data[:MAX_CHUNK])
        self.ops.chunk(MAX_CHUNK + 7, data[MAX_CHUNK : 2 * MAX_CHUNK])  # gap
        self.assertEqual(self.ops.status()["err"], E_OFFSET)
        link.write(CMD, bytes([4]))
        self.assertEqual(self.ops.status()["op"], 0)
        self.assertEqual(self.ops.list(), {})

    def test_replayed_chunk(self):
        data = self.mockup
        self.ops.upload_begin(NAME, len(data), crc32(data))
        self.ops.chunk(0, data[:MAX_CHUNK])
        self.ops.chunk(0, data[:MAX_CHUNK])  # replay
        self.assertEqual(self.ops.status()["err"], E_OFFSET)
        link.write(CMD, bytes([4]))

    def test_oversized_chunk(self):
        self.ops.upload_begin(NAME, 1000, 0)
        self.ops.chunk(0, bytes(MAX_CHUNK + 1))
        self.assertEqual(self.ops.status()["err"], E_COMMAND)
        link.write(CMD, bytes([4]))

    def test_crc_mismatch_leaves_no_file(self):
        data = self.mockup
        self.ops.upload_begin(NAME, len(data), crc32(data) ^ 1)
        for off in range(0, len(data), MAX_CHUNK):
            self.ops.chunk(off, data[off : off + MAX_CHUNK])
        self.assertProxyError(E_CRC, self.ops.command, 3)
        self.assertEqual(self.ops.list(), {})
        self.assertEqual(self.ops.status()["op"], 0)

    def test_incomplete_upload_leaves_no_file(self):
        data = self.mockup
        self.ops.upload_begin(NAME, len(data), crc32(data))
        self.ops.chunk(0, data[:MAX_CHUNK])
        self.assertProxyError(E_INCOMPLETE, self.ops.command, 3)
        self.assertEqual(self.ops.list(), {})

    def test_finish_or_chunk_without_upload(self):
        self.assertProxyError(E_NO_UPLOAD, self.ops.command, 3)
        self.ops.chunk(0, b"abc")
        self.assertEqual(self.ops.status()["err"], E_NO_UPLOAD)

    def test_second_transfer_is_busy(self):
        self.ops.upload_begin(NAME, 100, 0)
        self.assertProxyError(E_BUSY, self.ops.upload_begin, "other.zip", 100, 0)
        self.assertProxyError(E_BUSY, self.ops.delete, NAME)
        link.write(CMD, bytes([4]))
        self.assertEqual(self.ops.status()["op"], 0)

    def test_unknown_command(self):
        self.assertProxyError(E_COMMAND, self.ops.command, 99)

    def test_failed_replacement_keeps_original(self):
        # A replacement that fails package validation must leave the stored
        # file byte-identical and listed; nothing else may appear.
        self.ops.upload(NAME, self.mockup)
        with self.assertRaises(ProxyError) as ctx:
            self.ops.upload(NAME, b"not a zip archive at all" * 6, replace=True)
        self.assertEqual(ctx.exception.code, E_PACKAGE)
        self.assertEqual(self.ops.list(), {NAME: len(self.mockup)})
        self.assertEqual(self.ops.download(NAME), self.mockup)

    def test_successful_replacement_leaves_no_hidden_files(self):
        # Replacing goes through a backup; once done only the new file remains
        # and the old bytes are gone.
        self.ops.upload(NAME, self.mockup)
        smaller = build_mockup_zip(max(64, ARGS.size // 2))
        before = self.ops.storage()["used"]
        self.ops.upload(NAME, smaller, replace=True)
        self.assertEqual(self.ops.list(), {NAME: len(smaller)})
        self.assertEqual(self.ops.download(NAME), smaller)
        self.assertLessEqual(self.ops.storage()["used"], before)

    def test_non_package_upload_rejected_with_no_file(self):
        # Outer CRC32 is correct, bytes are not a ZIP: the proxy must refuse
        # to store it with PACKAGE and keep storage unchanged.
        junk = b"this is not a zip archive" * 8
        before = self.ops.list()
        with self.assertRaises(ProxyError) as ctx:
            self.ops.upload(NAME, junk)
        self.assertEqual(ctx.exception.code, E_PACKAGE)
        self.assertEqual(self.ops.list(), before)

    def test_valid_package_upload_accepted(self):
        # The in-memory mockup.zip is a real stored ZIP the app would accept:
        # the proxy must accept it (same bytes as the round-trip test uses).
        self.ops.upload(NAME, self.mockup)
        self.assertEqual(self.ops.list(), {NAME: len(self.mockup)})

    def test_max_length_name_roundtrip(self):
        # 63 chars is the legal maximum (59-char stem + ".zip"); a truncated
        # path would store an undeletable phantom file.
        name = "x" * 59 + ".zip"
        assert len(name) == 63
        self.ops.upload(name, self.mockup)
        self.assertEqual(self.ops.list(), {name: len(self.mockup)})
        self.assertEqual(self.ops.download(name), self.mockup)
        self.ops.delete(name)
        self.assertEqual(self.ops.list(), {})

    def test_disconnect_aborts_upload(self):
        data = self.mockup
        self.ops.upload_begin(NAME, len(data), crc32(data))
        self.ops.chunk(0, data[:MAX_CHUNK])
        link.reconnect()
        self.assertEqual(self.ops.status()["op"], 0)
        self.assertEqual(self.ops.list(), {})
        self.ops.upload(NAME, data)  # storage still usable afterwards
        self.assertEqual(self.ops.download(NAME), data)

    def test_disconnect_closes_open_download(self):
        # A download left open across a disconnect must be closed by the file
        # worker, otherwise the next connection is stuck BUSY on that handle.
        self.ops.upload(NAME, self.mockup)
        self.ops.command(6, self.ops.name_field(NAME))  # DOWNLOAD_BEGIN, no END
        self.assertEqual(self.ops.status()["op"], 2)
        link.reconnect()
        self.assertEqual(self.ops.status()["op"], 0)
        self.assertEqual(self.ops.download(NAME), self.mockup)
        self.ops.delete(NAME)  # BUSY here would mean the handle leaked


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", action="store_true", help="run against a fake proxy (harness check only)")
    parser.add_argument("--mac", help="proxy BLE address (default: scan by name)")
    parser.add_argument("--size", type=int, default=3000, help="firmware.bin bytes inside mockup.zip")
    ARGS, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
