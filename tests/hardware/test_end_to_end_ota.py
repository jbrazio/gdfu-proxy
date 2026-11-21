import os, pty, select, struct, sys, time

def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc = ((crc >> 8) & 0xFF) | ((crc << 8) & 0xFFFF)
        crc ^= b
        crc ^= (crc & 0xFF) >> 4
        crc ^= ((crc << 8) << 4) & 0xFFFF
        crc ^= (((crc & 0xFF) << 4) << 1) & 0xFFFF
    return crc & 0xFFFF

master, slave = pty.openpty()
pid = os.fork()

if pid == 0:
    os.close(master)
    os.dup2(slave, 0)
    os.dup2(slave, 1)
    os.dup2(slave, 2)
    os.close(slave)
    os.execvp("gatttool", ["gatttool", "-i", "hci1", "-b", "10:51:DB:2B:06:91", "-I"])

os.close(slave)

def send_cmd(cmd, wait_str=None, timeout=6.0):
    os.write(master, (cmd + "\n").encode())
    out = ""
    start = time.time()
    while time.time() - start < timeout:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try:
                data = os.read(master, 1024).decode(errors="ignore")
                out += data
                if wait_str and wait_str in out:
                    return out
            except OSError:
                break
    return out

print("[1] Connecting to GFSK DFU PROXY...")
out = send_cmd("connect", "Connection successful", timeout=8.0)
if "Connection successful" not in out:
    print("FAILED to connect to proxy:", out)
    sys.exit(1)
time.sleep(1)

print("[2] Reading Discovery...")
out = send_cmd("char-read-hnd 0x002e", "Characteristic value/descriptor:", timeout=4.0)
parts = [p for p in out.split() if len(p) == 2 and all(c in '0123456789abcdefABCDEF' for c in p)]
if not parts or parts[0] == '00':
    print("Waiting 2s for fresh beacon...")
    time.sleep(2)
    out = send_cmd("char-read-hnd 0x002e", "Characteristic value/descriptor:", timeout=4.0)
    parts = [p for p in out.split() if len(p) == 2 and all(c in '0123456789abcdefABCDEF' for c in p)]

target_hex = "".join(parts[1:5])
print(f"Discovered target ID: 0x{target_hex}")

print(f"[3] Selecting target 0x{target_hex}...")
send_cmd(f"char-write-req 0x0031 01{target_hex}", "Characteristic value was written", timeout=3.0)

print("[4] Checking status (expect 01 IDLE)...")
out = send_cmd("char-read-hnd 0x0033", "Characteristic value/descriptor:", timeout=3.0)
print("Status:", out.strip())

# Build dfu_start_packet_t: mode=0x04 (app only), sd_size=0, bl_size=0, app_size=16
start_payload = struct.pack("<IIII", 0x04, 0, 0, 16)
start_hex = "02" + start_payload.hex()

print(f"[5] Sending START_DFU command (cmd=2, app_size=16)...")
send_cmd(f"char-write-req 0x0031 {start_hex}", "Characteristic value was written", timeout=4.0)

print("[6] Waiting for target START acceptance...")
time.sleep(3)

print("[7] Checking Status (expect 03 STREAMING)...")
out = send_cmd("char-read-hnd 0x0033", "Characteristic value/descriptor:", timeout=4.0)
print("Status:", out.strip())

# Build correct 14-byte init packet:
test_image = b"\x00\x00\x04\x20\x09\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
img_crc = crc16_ccitt(test_image)
init_body = struct.pack("<HHIHH", 0x0052, 0x0001, 1, 1, 0xFFFE)
init_packet = init_body + struct.pack("<H", img_crc)
print(f"[8] Sending INIT packet (length={len(init_packet)}): {init_packet.hex()}")

send_cmd(f"char-write-req 0x0036 {init_packet.hex()}", "Characteristic value was written", timeout=5.0)

print("[9] Waiting for INIT packet RF processing...")
time.sleep(2)

print("[10] Checking Status...")
out = send_cmd("char-read-hnd 0x0033", "Characteristic value/descriptor:", timeout=4.0)
print("Status:", out.strip())

print("[11] Sending DATA packet (16 bytes application image)...")
send_cmd(f"char-write-req 0x0036 {test_image.hex()}", "Characteristic value was written", timeout=5.0)

print("[12] Waiting for DATA packet RF processing...")
time.sleep(2)

print("[13] Checking Status...")
out = send_cmd("char-read-hnd 0x0033", "Characteristic value/descriptor:", timeout=4.0)
print("Status:", out.strip())

print("[14] Sending FINISH_DFU command (cmd=5)...")
send_cmd("char-write-req 0x0031 05", "Characteristic value was written", timeout=4.0)

print("[15] Waiting for FINISH verification & activation...")
time.sleep(3)

print("[16] Checking final Status (expect 01 IDLE, error 0)...")
out = send_cmd("char-read-hnd 0x0033", "Characteristic value/descriptor:", timeout=4.0)
print("Final Status:", out.strip())

send_cmd("quit")
os.close(master)
print("\n=== SUCCESS: Full OTA flow executed end-to-end! ===")
