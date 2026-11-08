# NuOS v2 — PhoneVR link + CardboardXR Unity home on the board

## Context

v1 (custom distro + native Swift app) was reset. New architecture, per your direction:
- **Phone** = the XR device, running a **Unity 6 app based on CardboardXR** (Apache-2.0): keeps **environment, hand tracking (on-phone MediaPipe), room setup/anchors, passthrough, stereo lens distortion, and the paged app dock** — all rendering locally on the iPhone (zero-latency head motion).
- **Board** (existing Ubuntu 26.04, reused — its QuickSync HEVC encode is already proven) = the "PC": runs **Steam + desktop apps**, and a **PhoneVR-protocol server** that streams any app's window to the phone, where it appears as a **floating window above the app dock**.
- **The link** = PhoneVR's protocol, implemented fresh from its documented spec (clean-room, no GPL code): phone UDP-announces `{'p','v','r',0,ver}` on **:33333** every 500 ms → board connects TCP back to phone:33333, sends `PAIR_ACCEPT` + `HEADER_NALS` (SPS/PPS Annex-B) → phone opens TCP to board:**15243** → board streams per frame a **64-byte header** (pts, quat wxyz, NAL size, telemetry) + raw HEVC Annex-B AU → phone sends **36-byte pose** (quat, accel, ts) UDP on **51423** @60 Hz. We extend the TCP control with a small TLV for window-list/launch/switch.

## Board (Linux "OS" layer) — reuse existing Ubuntu

`setup/nuos2-setup.sh` (idempotent, run over SSH):
1. Steam: `apt install steam` (multiverse) + i386 runtime deps; verify launch on the board's KDE session.
2. VAAPI sanity: iHD driver + HEVC EncSliceLP (already proven — re-check).
3. Force **X11** KDE session (per-window XShm capture is reliable on X11; Wayland portals are v2).
4. Build + install `nuospvr-server` (systemd service) and `nuos-winlist` helper.
5. Keep `NuOS-25CF` AP profile (off by default).

`server/` (C++, small): `pvr_server.cpp` (protocol state machine above), `win_capture.cpp` (XShm per-window BGRA grab), encoder reuses the **proven libavcodec hevc_vaapi pattern** from v1 (documented in legacy/LEGACY_NOTES.md — pooled-surface upload, extern-C FFmpeg 8 headers). Control TLVs: `LIST_WINDOWS`, `LAUNCH(app)`, `STREAM_WINDOW(id)`, `STOP`.

## Phone (Unity 6 app from CardboardXR)

`unity/` — copy of brenpoly/CardboardXR (Unity 6000.6.8f1, Apache-2.0), then:
- **Keep**: Stereo stack (lens distortion + StereoRig), Hands (Sentis MediaPipe), room setup/anchors, passthrough, app-library dock (adapted to our catalog), viewer QR profile scanning.
- **Strip**: demo apps, 360-video, unused Android IMU.
- **Add `PVRLink` (C#)**: PhoneVR protocol client side (announcer, TCP 33333 server, 15243 video client parsing the 64-byte header, 51423 pose sender at 60 Hz, control TLVs).
- **Add `PVRDecoder` native plugin** (`PVRDecoder.mm`, ~200 lines): VideoToolbox decode → CVPixelBuffer → Unity native texture (drops into the iOS build like CardboardXR's own `.mm`).
- **Add `FloatingWindow`**: quad above the dock bound to a stream slot; dock entries = board apps (populated via LIST_WINDOWS/LAUNCH); pinch/gaze to select and launch.
- Pose stream: send ARKit head quaternion on 51423 (board logs it; used for input in v2).

## Build flow (no Unity license in CI)

1. You install **Unity 6000.6.8f1** (free Personal) on this Windows PC → open `unity/` → Build (iOS) → produces `Unity-iPhone` Xcode project.
2. Commit the generated Xcode project → GitHub Actions `macos-14` runner runs `xcodebuild` (no Unity/license needed) → unsigned IPA → release on `v*` tags → sideload as before.

## Repo layout (fresh tree, legacy/ untouched)

```
setup/nuos2-setup.sh, nuos2-services.md
server/ (CMakeLists, src/: pvr_server.cpp, win_capture.cpp, encoder/, proto/)
unity/  (CardboardXR-derived Unity project; PVRLink/, PVRDecoder, FloatingWindow/, dock)
.github/workflows/ios.yml (xcodebuild → IPA → release on tags)
docs/ (protocol-v2.md — PhoneVR framing + our TLV extension; setup guide)
legacy/ (untouched)
```

## Milestones & verification

1. **Board link proof**: `tools/fake-phone.py` (like v1's) performs announce→handshake→receives HEADER_NALS + decodable HEVC from a test pattern; ffmpeg-decode the capture and eyeball a PNG.
2. **Window streaming**: launch gedit/Steam → its window streams; decoded frame shows the app.
3. **Phone link**: NuosView-equivalent Unity app connects, shows the window stream on the FloatingWindow quad, dock launches apps.
4. **End-to-end**: phone in a cardboard viewer, launch Steam Big-Picture-equivalent window from the dock, see it floating above the dock.

## Risks (flagged up front)

- Unity 6000.6.8f1 must match exactly (CardboardXR is one week old, single-maintainer).
- X11 capture of hardware-accelerated games (vulkan/GL) works via compositor texture, but full-screen exclusive games may need a windowed-mode setting.
- iOS native plugin (VideoToolbox→Unity texture) is the riskiest phone piece; kept deliberately small.
- Steam i386 runtime needs ~5 GB disk (97 GB free ✓).
