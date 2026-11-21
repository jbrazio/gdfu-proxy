"""Two-target vicinity acceptance.

Self-skipping until a second flashed target is present. Pass the BLE MACs of
TWO proxies on argv (each proxy sees the same RF vicinity):

    python3 tests/hardware/test_two_targets.py <proxyA-mac> <proxyB-mac>

Procedure:
  1. Both proxies must list the same two target IDs with independent TTLs.
  2. Select target 1 through proxy A and target 2 through proxy B; both
     selections must succeed independently (no cross-talk ACKs).
  3. Reverse the mapping and confirm both still succeed.
"""

import os
import pty
import re
import select
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
    return master, pid


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
    global leftover
    leftover = ""
    os.write(master, (command + "\n").encode())
    return pump(master, wait)


def read_handle(master: int, handle: str) -> str:
    out = send(master, f"char-read-hnd {handle}")
    clean = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]|\r|\[K", "", out)
    match = re.search(r"value/descriptor:\s*((?:[0-9A-Fa-f]{2}\s*)+)", clean)
    return match.group(1).strip() if match else ""


def connect(mac: str) -> int:
    master, pid = open_gatttool(mac)
    send(master, "connect", 10)
    send(master, "mtu 247", 2)
    return master


leftover = ""


def discover_targets(master: int, seconds: float = 8.0) -> dict:
    """Poll DISCOVERY until two distinct live targets are seen; return id→ttl."""
    seen = {}
    end = time.time() + seconds
    while time.time() < end:
        raw = read_handle(master, "0x0030")
        fields = raw.split()
        if len(fields) < 1:
            # Absent/unresponsive proxy: empty read. Keep polling so the
            # harness reports a clean "not enough targets" failure instead.
            time.sleep(0.5)
            continue
        count = int(fields[0], 16)
        for index in range(count):
            base = 1 + index * 8
            fields = raw.split()
            if base + 8 > len(fields):
                break
            target_id = "".join(fields[base : base + 4])
            ttl = int("".join(fields[base + 6 : base + 8]), 16)
            if ttl > 0:
                seen[target_id] = ttl
        if len(seen) >= 2:
            return seen
        time.sleep(0.5)
    return seen


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_two_targets.py <proxyA-mac> <proxyB-mac>")
        return 2
    proxy_a, proxy_b = sys.argv[1:3]

    print("[1] Connecting to both proxies…")
    master_a = connect(proxy_a)
    master_b = connect(proxy_b)

    print("[2] Waiting for two distinct live targets…")
    seen_a = discover_targets(master_a)
    seen_b = discover_targets(master_b)
    print("proxy A targets:", seen_a)
    print("proxy B targets:", seen_b)
    if len(seen_a) < 2 or len(seen_b) < 2:
        print("FAIL: need two live targets visible to each proxy")
        return 1
    common = sorted(set(seen_a) & set(seen_b))
    if len(common) < 2:
        print("FAIL: proxies do not share two common targets")
        return 1
    target_one, target_two = common[0], common[1]

    print("[3] Independent selection: A→t1, B→t2")
    send(master_a, f"char-write-req 0x0033 01{target_one}", 2)
    send(master_b, f"char-write-req 0x0033 01{target_two}", 2)
    time.sleep(1.5)
    status_a = read_handle(master_a, "0x0035")
    status_b = read_handle(master_b, "0x0035")
    print("A:", status_a, "B:", status_b)
    ok_a = status_a.startswith("01") and int(status_a.split()[9], 16) == 0
    ok_b = status_b.startswith("01") and int(status_b.split()[9], 16) == 0
    if not (ok_a and ok_b):
        print("FAIL: independent selection not accepted")
        return 1

    print("[4] Reverse mapping: A→t2, B→t1 (after abort both)")
    send(master_a, "char-write-req 0x0033 03", 2)
    send(master_b, "char-write-req 0x0033 03", 2)
    time.sleep(2)
    send(master_a, f"char-write-req 0x0033 01{target_two}", 2)
    send(master_b, f"char-write-req 0x0033 01{target_one}", 2)
    time.sleep(1.5)
    status_a = read_handle(master_a, "0x0035")
    status_b = read_handle(master_b, "0x0035")
    ok_a = status_a.startswith("01") and int(status_a.split()[9], 16) == 0
    ok_b = status_b.startswith("01") and int(status_b.split()[9], 16) == 0
    print("A:", status_a, "B:", status_b)
    if not (ok_a and ok_b):
        print("FAIL: reversed independent selection not accepted")
        return 1

    send(master_a, "char-write-req 0x0033 03", 2)
    send(master_b, "char-write-req 0x0033 03", 2)
    print("TWO-TARGET VICINITY ACCEPTANCE: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
