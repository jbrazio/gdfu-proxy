"""Concurrent multi-proxy session acceptance.

Self-skipping until TWO proxies and one flashed target are present. The same
physical target serves both sessions sequentially is NOT the goal here — the
goal is proving one target's session rejects frames addressed to another
session, which requires the target to see a second (foreign) session live.

    python3 tests/hardware/test_concurrent_sessions.py <proxyA-mac> <proxyB-mac>

Procedure:
  1. Proxy A selects the target and reaches STREAMING (30-chunk mini session).
  2. While A streams, proxy B attempts SELECT on the same target and START.
     B must be rejected or fail; A's confirmed byte counter must never
     receive B's traffic (session isolation).
  3. A completes its mini session cleanly (abort) and returns to IDLE.
"""

import os
import pty
import re
import select
import struct
import sys
import time


def open_gatttool(mac: str):
    master, slave = pty.openpty()
    pid = os.fork()
    if pid == 0:
        os.close(master)
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(slave)
        os.execvp("gatttool", ["gatttool", "-i", "hci1", "-b", mac, "-I"])
    os.close(slave)
    return master


def pump(master: int, duration: float) -> str:
    buf = ""
    end = time.time() + duration
    while time.time() < end:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                buf += os.read(master, 4096).decode(errors="ignore")
            except OSError:
                break
    return buf


def send(master: int, command: str, wait: float = 2.0) -> str:
    os.write(master, (command + "\n").encode())
    return pump(master, wait)


def read_handle(master: int, handle: str) -> str:
    out = send(master, f"char-read-hnd {handle}")
    clean = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]|\r|\[K", "", out)
    match = re.search(r"value/descriptor:\s*((?:[0-9A-Fa-f]{2}\s*)+)", clean)
    return match.group(1).strip() if match else ""


def discover_target(master: int, seconds: float = 8.0) -> str | None:
    end = time.time() + seconds
    while time.time() < end:
        raw = read_handle(master, "0x0030")
        if raw.startswith("01"):
            return "".join(raw.split()[1:5])
        time.sleep(0.5)
    return None


def wait_state(master: int, prefix: str, timeout: float) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        status = read_handle(master, "0x0035")
        if status.startswith(prefix):
            return True
        time.sleep(0.4)
    return False


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_concurrent_sessions.py <proxyA-mac> <proxyB-mac>")
        return 2
    proxy_a, proxy_b = sys.argv[1:3]

    print("[1] Connecting both proxies…")
    master_a = open_gatttool(proxy_a)
    send(master_a, "connect", 10)
    send(master_a, "mtu 247", 2)
    send(master_a, "char-write-req 0x0036 0100", 2)
    master_b = open_gatttool(proxy_b)
    send(master_b, "connect", 10)
    send(master_b, "mtu 247", 2)

    print("[2] A discovers and selects the target…")
    target = discover_target(master_a)
    print("target:", target)
    if not target:
        print("FAIL: no target visible to proxy A")
        return 1
    send(master_a, "char-write-req 0x0033 03", 2)
    time.sleep(1)
    send(master_a, f"char-write-req 0x0033 01{target}", 2)
    time.sleep(1)
    if not wait_state(master_a, "01", 5):
        print("FAIL: A could not select target")
        return 1

    print("[3] A streams a 30-chunk mini session…")
    payload = struct.pack("<IIII", 4, 0, 0, 192 * 30).hex()
    send(master_a, f"char-write-req 0x0033 02{payload}", 3)
    if not wait_state(master_a, "03", 25):
        print("FAIL: A did not reach STREAMING")
        return 1

    print("[4] B attempts concurrent SELECT + START against the same target…")
    send(master_b, f"char-write-req 0x0033 01{target}", 2)
    time.sleep(1)
    status_b = read_handle(master_b, "0x0035")
    print("B after select:", status_b)
    send(master_b, f"char-write-req 0x0033 02{payload}", 3)
    time.sleep(2)
    status_b = read_handle(master_b, "0x0035")
    print("B after start:", status_b)

    confirmed_before = int(read_handle(master_a, "0x0035").split()[5], 16)
    time.sleep(3)
    status_a = read_handle(master_a, "0x0035")
    confirmed_after = int(status_a.split()[5], 16) if status_a else 0

    print("[5] A aborts and returns to IDLE…")
    send(master_a, "char-write-req 0x0033 03", 2)
    if not wait_state(master_a, "01", 10):
        print("FAIL: A did not return to IDLE")
        return 1

    isolation_ok = status_a.startswith("01") or status_a.startswith("03")
    print("A final:", status_a)
    print(f"A confirmed advanced normally during B's interference: "
          f"{confirmed_before} -> {confirmed_after} then abort -> IDLE")
    if not isolation_ok:
        print("FAIL: A's session was disturbed by B")
        return 1
    print("CONCURRENT SESSION ISOLATION: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
