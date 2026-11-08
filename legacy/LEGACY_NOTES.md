# NuOS — Legacy Notes (pre-reset)

Preserved from the first iteration (full history: see `git log`, releases
v0.1.0–v0.1.4 on GitHub). This folder is the "keep" pile before the reset.

## What was proven working on real hardware

- **Board**: Lenovo IdeaPad Chromebook (i3-1215U, Alder Lake), Ubuntu 26.04,
  KDE Plasma + SDDM on the internal panel, zram, power tuning.
- **Video pipeline (end-to-end verified)**: Godot 4.3 XR home rendered under
  Xvfb → `/dev/shm/nuos_frames` ring (BGRA, seqlock) → `nuos-frameudp` bridge
  → `nuos-stream` encoding via **libavcodec `hevc_vaapi`** (QuickSync
  VAEntrypointEncSliceLP path, CBR) → UDP fragment stream to the phone.
  The exact bitstream the phone received was decoded and inspected
  (see `evidence/dec1.png` — the XR home, correct).
- **Wire protocol** (`nuos_proto.h` + `PROTOCOL.md`): pose 60 Hz UDP,
  hands 30 Hz, HEVC fragments + XOR FEC, TCP control with HELLO/SESSION.
  Small, proven, reusable — kept verbatim in this folder.
- **Board network**: home Wi-Fi client + registered-but-off AP profile
  `NuOS-25CF` (NetworkManager `shared` mode, 10.66.0.1/24), mDNS
  `_nuos-xr._tcp` advertising (works: Windows resolves the board's .local name).

## Board access (PRIVATE repo — rotate if ever leaked)

- IP: `192.168.1.197` (DHCP; hostname `a-Taniks.local`)
- User: `a`, password: `1`, sudo: password `1`
- SSH keypair for the board: `C:\Users\user\.ssh\nuos_board` (+ `.pub`)
- AP credentials live on the board in `/var/lib/nuos/ap.conf` and `~/.nuos/ap.conf`
- Setup scripts and exact service units: `git log` (search "Stage"), or the
  board itself (`/etc/systemd/system/nuos-*.service`).

## Gremlins learned the hard way (will bite again if forgotten)

1. **"Text file busy"**: `sudo cp` over a *running* service binary fails
   silently under `&&` chains. Deploy pattern: `systemctl stop` → cp →
   **verify md5** → start.
2. **systemd-tmpfiles deletes stale `/dev/shm` files**: `nuos_frames` has
   idle gaps when no headset connects → ring file vanishes mid-pipeline.
   Fixed with `/etc/tmpfiles.d/nuos-shm.conf` containing `x /dev/shm/nuos_*`.
3. **FFmpeg 8 headers (Ubuntu 26.04) do not self-wrap in `extern "C"`** —
   C++ code linking libav* must wrap the includes or every symbol mangles.
4. **Godot `StreamPeerTCP.put_u32` endianness is unreliable** — use
   `PackedByteArray.encode_u32` (always LE) for wire headers.
5. **iHD/VAAPI upload**: `av_hwframe_transfer_data` into a hand-referenced
   dst frame fails EINVAL; the dst must come from the pool via
   `av_hwframe_get_buffer`. `av_frame_make_writable` on an empty hw frame
   fails EINVAL (and is unnecessary).
6. **Hand-rolled VAEntrypointEncSliceLP** (manual SPS/PPS/picture/slice
   buffers) emitted empty coded buffers on iHD where libavcodec's
   hevc_vaapi worked — prefer libavcodec.
7. **Ubuntu's sddm.service has an ExecStartPre guard** reading
   `/etc/X11/default-display-manager` — installing SDDM doesn't flip that
   file; the DM silently never starts until you write `/usr/bin/sddm` into it.
8. **Metal unbound-texture sampling of YCbCr yields green**:
   `yuv2rgb(0, 0, 0)` = green. Any "green screen" in a Metal YCbCr renderer
   usually means nil/missing textures, not corrupt video.
9. **A HEVC decoder must see every frame in order** — dropping "stale"
   P-frames between IDRs produces VideoToolbox green placeholders.
10. **`pkill -f nuos-home` kills your own SSH command line** (pattern
    matches it) — use exact-name pkill.
11. Debian trixie ships only the *signed* `systemd-bootx64.efi.signed`;
    Ubuntu 26.04 has no `libva-utils` package (it's `vainfo` from
    `libva-utils`? — actually `vainfo` ships in `vainfo` pkg on Ubuntu).

## Phone app state at reset

- v0.1.4 release: connects by IP (manual field, persisted), keeps screen
  awake, Diagnostics back button, decode counters shown.
- The green screen was fixed at the shader level (nil-texture guard), and
  `onVideoFrame` now fires for every assembled frame — but end-to-end video
  was **never visually confirmed** (user reported green through v0.1.2;
  v0.1.4's decode counters were never read back before the reset).
- Known unfixed: XOR FEC recovery pads recovered fragments to the group max
  (length corruption under packet loss); "link: idle" HUD label unwired.

## Board cleanup status at reset

Board services (`nuos-home`, `nuos-frameudp`, `nuos-stream`, `nuos-tracker`,
`nuos-handbus`, `nuos-power-tune`) were left **installed and running** —
they do not depend on this repo. Stop with
`sudo systemctl disable --now nuos-*` if the new direction wants a clean board.
