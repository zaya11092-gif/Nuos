# NuOS — DIY Standalone VR Headset Platform

## Context

Build a complete DIY standalone VR headset platform in the empty `Nuos/` workspace: a custom Linux distro ("NuOS") that runs on a motherboard mounted to a wireless headset frame, plus an iOS app that turns an iPhone 15 into the headset's display and tracking pod.

**Pinned hardware:**
- **Motherboard**: Lenovo IdeaPad Chromebook (i3-1215U Alder Lake, 2P+4E cores, 8GB RAM, Intel UHD Graphics 64EU) flashed with **MrChromebox full UEFI coreboot firmware** → boots our distro like a standard PC, no ChromeOS remnants or BSP needed. Gen12 QuickSync media engine = hardware HEVC encode (low-power VDENC mode).
- **iPhone 15 (base)**: 48MP main + 12MP ultrawide (120°), 60Hz fixed display.
- **Cameras (your choice: ARKit-first + probe)**: ARKit (main camera) does 6DoF SLAM and provides frames for passthrough; Vision framework does hand tracking on the same frames; the app ships a diagnostics screen that queries `supportedMultiCamSessionConfigurations` on your unit and auto-enables the ultrawide for passthrough/hands if that unit allows it. Fallback path (main camera does everything) always works.
- **Razer Kiyo** (720p60, USB): forward-mounted on frame, plugged into the motherboard — lightweight visual odometry fused (EKF) with ARKit pose to correct positional drift.
- **XR home v1**: launcher + floating 2D app windows, passthrough backdrop, system dashboard (battery/thermals/net/tracking).

**This session delivers**: WSL installed, the full monorepo scaffolded with working code for every component, GitHub Actions workflows building the unsigned IPA + distro image, and tests runnable in WSL. Hardware-in-the-loop steps (USB-booting the Chromebook, on-device camera probe) are documented for when hardware is assembled.

## Architecture

```
┌─ Headset frame ──────────────────────────────────────────────┐
│ Chromebook board (i3-1215U, 8GB) — NuOS distro                │
│  ├─ nuos-home (Godot 4 / OpenXR) — launcher, windows, dash    │
│  ├─ Monado — OpenXR runtime; nuos_posed driver (pose from shm)│
│  ├─ nuos-stream — grabs rendered frame → VAAPI HEVC → WiFi    │
│  ├─ nuos-tracker — Kiyo VO (KLT/PnP) ⊕ ARKit pose → EKF → shm │
│  ├─ nuos-handbus — hand joints from phone → XR apps           │
│  └─ Wi-Fi AP mode + mDNS; laptop panel OFF (power)            │
│         ↕ UDP pose/hands ↑  ·  HEVC video ↓  ·  TCP control   │
│ Razer Kiyo (720p60 USB)        iPhone 15 in cradle = display  │
└───────────────────────────────────────────────────────────────┘
iPhone 15 "NuosView" app:
  ARKit VIO → pose @60Hz (UDP) · Vision hands @30Hz · passthrough frames
  VideoToolbox HEVC decode → Metal: timewarp (freshest pose) + passthrough
  backdrop composite + stereo pre-distortion → display @60Hz
```

Key design decisions (power + latency driven):
- **Passthrough composited on the phone** — virtual layer arrives via video; passthrough blended locally on-device. Avoids double-encoding camera video over WiFi.
- **Late-stage reprojection (timewarp) on the phone** with the freshest ARKit pose — makes a streamed 60Hz display tolerable (~30–45ms effective motion-to-photon; fine for a home/desktop environment, not fast gaming).
- **Power budget on the board** (~12–20W under load): i3-1215U capped via intel_pstate/RAPL, panel off (iPhone is the display), VDENC low-power HEVC encode (~2–4W), zram for the 8GB RAM, minimal userland, services core-pinned.
- Hardware encode (QuickSync) / decode (VideoToolbox) both ends; minimized camera resolutions; fixed 60Hz pacing.

## Repo layout (all new, under `Nuos/`)

```
├─ README.md, docs/ (architecture, power-budget, hardware-assembly,
│    ios-camera-constraints, calibration)
├─ proto/            nuos_proto.h + PROTOCOL.md (pose/hands/video-frag/control packets)
├─ distro/           build-image.sh (mmdebstrap Debian 13 x86_64), overlay/ (systemd
│                    units, firstboot AP+mdns, panel-off cmdline), packages/
│                    (metapackage defs), board/ideapad-chromebook/profile.conf
├─ platform/         C++ daemons: nuos-tracker (V4L2 Kiyo → KLT VO → EKF → shm),
│                    nuos-stream (VAAPI HEVC + UDP/FEC + control), nuos-handbus,
│                    monado-nuos-driver, tests/ (replay fixtures)
├─ xr-home/          Godot 4 OpenXR project: launcher grid, floating panels,
│                    dashboard, passthrough backdrop, nuos_client.gd
├─ ios/NuosView/     project.yml (xcodegen) + Swift: Tracking/ (ARKit→UDP 60Hz),
│                    Hands/ (Vision, 30Hz), Camera/ (ARFrame feed + UltraWideProbe),
│                    Video/ (VideoToolbox low-latency HEVC), Render/ (Metal timewarp,
│                    composite, stereo pre-distortion), Net/ (mDNS, UDP+FEC,
│                    jitter buffer), App/ (incl. DiagnosticsView w/ camera probe)
├─ .github/workflows/ ios.yml (macos runner → unsigned IPA, optional signed via
│                    secrets), platform.yml, distro.yml (+ qemu smoke test)
└─ tools/            qemu-smoke.sh, replay fixtures
```

Distro specifics for this board: mainline kernel + linux-firmware (i915 GuC/HuC for Alder Lake), `intel-media-va-driver` (iHD) for QuickSync, board profile sets `video=eDP-1:off` kernel cmdline, PL1/PL2 power caps, intel AX-series Wi-Fi firmware for AP mode. Image stays generic x86_64 so other boards work later by adding a profile.

## Implementation order

1. **WSL**: `wsl --install -d Ubuntu-24.04` (needs elevation/reboot), then toolchain inside WSL: `build-essential cmake meson ninja-build mmdebstrap qemu-system-x86 ffmpeg libopencv-dev libva-dev godot` etc. Mirrored networking mode for the later phone↔WSL LAN test.
2. **proto/** — wire protocol spec + `nuos_proto.h` (single source of truth; Swift and GDScript mirrors written to match).
3. **ios/NuosView/** — the full app as specced above; xcodegen project so no binary .xcodeproj in git. Compiles in CI.
4. **CI: ios.yml** — macos-15, `xcodegen` + `xcodebuild` with `CODE_SIGNING_ALLOWED=NO` → unsigned IPA artifact; optional signed build via `P12`/`mobileprovision` secrets later. Sideloading path documented (free Apple ID + Sideloadly/AltStore, 7-day re-sign; camera/ARKit work on free provisioning).
5. **platform/** — nuos-tracker (Kiyo V4L2 720p60, grayscale downscale, sparse KLT + PnP odometry, EKF loosely-coupled fusion with ARKit pose, publish @90Hz to shm, SCHED_FIFO, core-pinned), nuos-stream, nuos-handbus, Monado driver plugin. Replay-based tests.
6. **xr-home/** — Godot 4 OpenXR launcher (floating panels, dashboard fed by a local status bus, passthrough backdrop fed from stream/tracker state).
7. **distro/** — mmdebstrap image builder + board profile as above, systemd `nuos.target` bringing up AP mode + mDNS + services, power tuning, `tools/qemu-smoke.sh` boots the image in QEMU and asserts services reach healthy state.
8. **CI: platform.yml, distro.yml** — ubuntu runners build daemons + image, run replay tests + qemu smoke, upload artifacts.
9. **docs/** — power budget table, assembly guide (Chromebook gutting + USB boot via MrChromebox, Kiyo + phone cradle mounting), iOS camera-constraint notes incl. probe result template, calibration guide.

## Verification

- **Distro**: `build-image.sh` produces a raw image in WSL; `qemu-smoke.sh` boots it, asserts `nuos.target` active and tracker/stream/home services healthy. USB-boot on the Chromebook documented (MrChromebox UEFI boot menu).
- **Tracking**: replay test feeds a sample clip through nuos-tracker; assert smooth, finite fused pose output.
- **XR home**: Godot headless import + Linux export builds clean in CI.
- **iOS**: `ios.yml` green on push → unsigned IPA artifact; you sideload and open Diagnostics to see the camera-probe result and live ARKit/hand-tracking HUD.
- **End-to-end smoke**: run nuos-stream + tracker in WSL (mirrored networking), phone on same Wi-Fi discovers via mDNS, decodes a test pattern from the XR home with live timewarp.

## Known constraints (stated up front)

- Base iPhone 15 is 60Hz fixed + 64EU iGPU → home/desktop-class XR experience (windowed UI, dashboards, media), not heavy 3D gaming.
- Wide+ultrawide simultaneous capture is generally unsupported by iOS on this device class; the probe screen states exactly what your unit allows. The always-works path (ARKit main camera for SLAM+passthrough+hands) is the default.
- Kiyo fusion is loose-coupled EKF drift correction (ARKit is primary) — not research-grade tight coupling.
- 8GB RAM: fine for this stack with zram + minimal userland; documented in power-budget.md.
- MrChromebox full ROM firmware (write-protect disabled) is assumed installed — recovery notes included.