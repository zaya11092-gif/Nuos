# NuOS — DIY Standalone VR Headset Platform

A custom Linux distro + iOS app that turn a gutted Chromebook board and an
iPhone 15 into a standalone VR headset: the board renders and streams the XR
home; the iPhone is the display, 6DoF tracking pod (ARKit SLAM), passthrough
camera, and hand tracker; a Razer Kiyo webcam adds odometry for drift
correction. All wireless, power-optimized.

```
┌─ Headset frame ──────────────────────────────────────────────┐
│ Lenovo Chromebook board (i3-1215U, 8GB, MrChromebox UEFI)     │
│  nuos-home (Godot/OpenXR) · Monado · nuos-stream (VAAPI HEVC) │
│  nuos-tracker (Kiyo VO ⊕ ARKit pose → EKF) · nuos-handbus     │
│  Wi-Fi AP 10.66.0.1 · mDNS _nuos-xr._tcp · panel OFF          │
│         ↕ pose/hands ↑ · HEVC ↓ · TCP control  (see proto/)   │
│ Razer Kiyo (720p60 USB)      iPhone 15 in cradle = display    │
└───────────────────────────────────────────────────────────────┘
```

## Repo layout

| Path | What |
|---|---|
| `proto/` | Wire protocol — the single source of truth (`nuos_proto.h`, `PROTOCOL.md`, `BRIDGE_API.md`) |
| `ios/NuosView/` | iPhone app: ARKit VIO → UDP pose, Vision hand tracking, VideoToolbox HEVC decode, Metal timewarp + stereo pre-distortion, ultrawide camera probe |
| `platform/` | Board daemons: tracker (fusion), stream (VAAPI HEVC + control), handbus, Monado driver, Godot bridge GDExtension |
| `xr-home/` | Godot 4.3 XR launcher: app grid, floating panels, dashboard |
| `distro/` | Debian 13 image builder (mmdebstrap), systemd overlay, board profile |
| `tools/` | `qemu-smoke.sh` (boot test), `make-usb.sh` (flash) |
| `.github/workflows/` | CI: `ios.yml` (unsigned IPA), `platform.yml` (daemons+tests), `distro.yml` (image+QEMU smoke), `home.yml` (Godot export) |
| `docs/` | power budget, hardware assembly, iOS camera constraints, calibration |

## Quick start

1. **Build the iOS app**: push to GitHub → `ios.yml` artifact `NuosView.ipa` → sideload with Sideloadly/AltStore (free Apple ID, 7-day resign).
2. **Build the distro** (WSL/Linux):
   - **Installer ISO (recommended)**: `distro/build-image.sh --iso --board ideapad-chromebook` → flash `nuos.iso` to USB → boot the Chromebook via the MrChromebox UEFI menu → an interactive TUI installer (whiptail) walks you through target-disk selection, Wi-Fi AP name/passphrase, and root password, then installs everything.
   - **Raw image (unattended)**: `distro/build-image.sh --out nuos.img --board ideapad-chromebook` → `tools/make-usb.sh`. First boot prints AP credentials on serial.
   - CI artifacts: `distro.yml` uploads `nuos-image` (raw) and `nuos-installer-iso` on every push to `distro/**`.
3. **Board binaries**: build `platform/` (see `platform/README.md`) and export `xr-home` (`home.yml` artifact), copy into the image via `--bins` / `--home-dir`.
4. **Smoke test without hardware**: `tools/qemu-smoke.sh nuos.img`.
5. Phone connects over the board's Wi-Fi (mDNS), Diagnostics screen shows the camera probe + live tracking; session flows per `proto/PROTOCOL.md`.

## Known constraints

- iPhone 15 is 60Hz fixed; 64EU iGPU → home/desktop-class XR, not fast gaming.
- iOS rarely allows simultaneous wide+ultrawide capture; the app probes at
  runtime (see `docs/ios-camera-constraints.md`). The always-works path is
  ARKit's main camera for SLAM + passthrough + hands.
- Kiyo fusion is loose-coupled drift correction (ARKit primary, metric scale).
- Uncompiled-on-Windows code (Swift/Metal/VAAPI paths) expects minor fixups on
  first real builds — CI is the first compile gate.
