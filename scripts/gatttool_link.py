#!/usr/bin/env python3
"""gatttool link: legacy interactive-gatttool transport for hardware tests.

Extracted verbatim from flasher_async.py before the Bleak rewrite (b3e97cd6~1).
Only tests/hardware/* consumes it; the flasher itself runs on Bleak now.
"""

import os
import pty
import re
import select
import subprocess
import time

def crc16_nordic(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc = ((crc >> 8) & 0xFF) | ((crc << 8) & 0xFFFF)
        crc ^= b
        crc ^= (crc & 0xFF) >> 4
        crc ^= ((crc << 8) << 4) & 0xFFFF
        crc ^= (((crc & 0xFF) << 4) << 1) & 0xFFFF
    return crc & 0xFFFF


PROXY_NAME = "GFSK DFU PROXY"
HCI_DEV = None


def find_proxy_mac(adapter: str = HCI_DEV, name: str = PROXY_NAME, timeout: float = 12.0) -> str:
    """Scan BLE for a device advertising PROXY_NAME; return its MAC address.

    A test-rig convenience: proxies come and go, so discovery is by name and
    the MAC is only used to open the GATT connection. bluetoothctl only prints
    [NEW] lines on first sighting, so the authoritative list comes from
    `bluetoothctl devices` after the scan refreshes the cache.
    """
    subprocess.run(
        ["bluetoothctl", "--timeout", str(int(timeout)), "scan", "on"],
        capture_output=True, text=True, timeout=timeout + 10,
    )
    devices = subprocess.run(
        ["bluetoothctl", "devices"], capture_output=True, text=True, timeout=15,
    )
    for line in devices.stdout.splitlines():
        if name.lower() in line.lower():
            for token in line.split():
                if token.count(":") == 5 and all(len(p) == 2 for p in token.split(":")):
                    return token.upper()
    raise SystemExit(f"ERROR: no BLE device advertising '{name}' found in {timeout:.0f}s")



class AsyncGatt:
    def __init__(self, mac: str, adapter: str = HCI_DEV):
        self.mac = mac
        self.adapter = adapter
        self.master, slave = pty.openpty()
        self.pid = os.fork()
        if self.pid == 0:
            os.close(self.master)
            os.dup2(slave, 0)
            os.dup2(slave, 1)
            os.dup2(slave, 2)
            os.close(slave)
            command = ["gatttool"]
            if self.adapter:
                command += ["-i", self.adapter]
            os.execvp("gatttool", [*command, "-b", self.mac, "-I"])
        os.close(slave)
        self.buf = ""

    def send_line(self, line: str):
        os.write(self.master, (line + "\n").encode())

    def read_until(self, pattern: str, timeout: float = 6.0) -> str:
        start = time.time()
        while time.time() - start < timeout:
            if pattern in self.buf:
                idx = self.buf.index(pattern) + len(pattern)
                out = self.buf[:idx]
                self.buf = self.buf[idx:]
                return out
            r, _, _ = select.select([self.master], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(self.master, 2048).decode(errors="ignore")
                    self.buf += chunk
                except OSError:
                    break
        return ""

    def send_and_wait(self, command: str, response: str, timeout: float = 6.0) -> str:
        self.buf = ""
        self.send_line(command)
        start = time.time()
        while time.time() - start < timeout:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if r:
                try:
                    self.buf += os.read(self.master, 2048).decode(errors="ignore")
                except OSError:
                    break
            if response in self.buf:
                after = self.buf.split(response, 1)[1]
                if "[LE]>" in after and "Notification handle" not in after.split("[LE]>", 1)[0]:
                    result = self.buf
                    self.buf = ""
                    return result
        result = self.buf
        self.buf = ""
        return result

    def read_notification(self, timeout: float = 3.0):
        output = self.read_until("Notification handle = 0x0033 value:", timeout=timeout)
        if not output:
            return None
        return self.read_until("\n", timeout=1.0)

    def read_handle(self, handle: str) -> bytes:
        output = self.send_and_wait(f"char-read-hnd {handle}", "Characteristic value/descriptor:")
        match = re.search(r"Characteristic value/descriptor:\s*((?:[0-9A-Fa-f]{2}\s*)+)", output)
        return bytes.fromhex(match.group(1)) if match else b""

    def write_req(self, handle: str, data: str) -> bool:
        return "Characteristic value was written" in self.send_and_wait(
            f"char-write-req {handle} {data}", "Characteristic value was written"
        )

    def write_cmd(self, handle: str, data: str):
        self.send_line(f"char-write-cmd {handle} {data}")
        self.buf = ""

    def close(self):
        try:
            self.send_line("quit")
        except Exception:
            pass
        os.close(self.master)
        try:
            os.waitpid(self.pid, 0)
        except Exception:
            pass
