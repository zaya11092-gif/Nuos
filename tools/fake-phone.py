#!/usr/bin/env python3
# NuOS v2 fake phone: speaks the PhoneVR-derived protocol for board-side tests.
# - UDP-announces {'p','v','r',0,ver} on :33333 (~2 Hz)
# - Listens TCP :33333; board connects; we receive PAIR_ACCEPT + HEADER_NALS
# - Sends NuOS v2 control TLVs (LIST_WINDOWS / STREAM_WINDOW)
# - Connects TCP to board:15243, receives 64B header + HEVC AUs -> dump file
# - Sends 36B pose UDP on :51423 @60 Hz
#
# Usage: fake-phone.py [board_ip] [dump_path] [duration_s]
import socket, struct, sys, threading, time, json
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

BOARD = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
DUMP = sys.argv[2] if len(sys.argv) > 2 else "/tmp/fake-phone.265"
DUR = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0

def tlv(t, payload=b""):
    return bytes([t]) + struct.pack("<I", len(payload)) + payload

running = True

def announcer():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    pkt = b"pvr" + bytes([0]) + struct.pack("<I", (2 << 24))
    while running:
        try: s.sendto(pkt, (BOARD, 33333))  # unicast only: one source IP
        except OSError: pass
        time.sleep(0.5)

threading.Thread(target=announcer, daemon=True).start()

def pose():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pkt = struct.pack("<4f3fxxxxq", 0, 0, 0, 1, 0, 0, 0, int(time.time_ns()))
    while running:
        s.sendto(pkt, (BOARD, 51423))
        time.sleep(1 / 60)

threading.Thread(target=pose, daemon=True).start()

ctl = socket.socket()
ctl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ctl.bind(("0.0.0.0", 33333))
ctl.listen(1)
print("[fake-phone] waiting for board TCP connect on 33333 ...")
ctl.settimeout(DUR + 5)
conn, addr = ctl.accept()
print(f"[fake-phone] board control from {addr}")

def recv_msg():
    h = b""
    while len(h) < 6:
        c = conn.recv(6 - len(h))
        if not c: return None, None
        h += c
    if h[:3] != b"pvr": raise ValueError("bad magic " + h.hex())
    t = h[3]; n = struct.unpack("<H", h[4:6])[0]
    p = b""
    while len(p) < n:
        c = conn.recv(n - len(p))
        if not c: raise ValueError("short payload")
        p += c
    return t, p

t, p = recv_msg()
print(f"[fake-phone] msg type={t} len={len(p) if p else 0}")
assert t == 3, "expected PAIR_ACCEPT"
print("[fake-phone] PAIR_ACCEPT ok")
t, header_nals = recv_msg()
assert t == 5, "expected HEADER_NALS"
print(f"[fake-phone] HEADER_NALS {len(header_nals)} bytes")

def list_windows():
    conn.sendall(tlv(0x10))
    t, p = recv_msg()
    if t == 4:
        try: return json.loads(p.decode())
        except Exception as e: print("windows parse fail:", e); return []
    return []

wins = list_windows()
if not wins:
    print("[fake-phone] no windows; launching xterm via LAUNCH")
    conn.sendall(tlv(0x11, b"xterm -title NuOS-Test"))
    t, p = recv_msg()  # ack
    time.sleep(2.5)
    wins = list_windows()
    print(f"[fake-phone] windows now: {len(wins)}")
for w in wins[:6]:
    print("   ", w)

vid = socket.create_connection((BOARD, 15243), timeout=5)
vid.settimeout(1.0)
print("[fake-phone] connected to video 15243")
target = wins[0]["id"] if wins else 0
conn.sendall(tlv(0x12, struct.pack("<I", target)))
print(f"[fake-phone] STREAM_WINDOW -> {target}")

au_bytes = bytearray()
header_bytes = bytearray(header_nals)
n_au = 0
end = time.time() + DUR
while time.time() < end and n_au < 60:
    try:
        d = vid.recv(262144)
        if not d: break
        buf = bytearray(d)
    except socket.timeout:
        continue
    while len(buf) >= 64:
        nalsz = struct.unpack("<I", buf[24:28])[0]
        if nalsz > 4_000_000:
            print("[fake-phone] insane NAL size, resync break"); buf.clear(); break
        total = 64 + nalsz
        if len(buf) < total: break
        au = bytes(buf[64:total])
        del buf[:total]
        n_au += 1
        if n_au == 1:
            print(f"[fake-phone] first AU: {len(au)} bytes")
        au_bytes += au

running = False
open(DUMP, "wb").write(header_bytes + au_bytes)
print(f"[fake-phone] done: {n_au} AUs -> {DUMP} ({len(header_bytes)+len(au_bytes)} bytes)")
conn.close(); vid.close()
