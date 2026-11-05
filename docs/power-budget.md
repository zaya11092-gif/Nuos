# NuOS Power Budget

Target: everything battery-powered on the headset frame.

| Component | Idle | Typical | Notes |
|---|---|---|---|
| i3-1215U board (capped PL1 15W) | 3–5 W | 12–20 W | `nuos-power-tune` sets intel_pstate max 70% + RAPL limit |
| iGPU render (UHD 64EU, home scene) | — | 3–6 W | windowed UI scene, 60 Hz |
| QuickSync HEVC encode (VDENC LP) | — | 2–4 W | 2560×720 SBS @ 60, 15 Mbps |
| Laptop panel (eDP) | **0 W** | **0 W** | forced off in kernel cmdline (`video=eDP-1:off`) |
| Razer Kiyo (720p60) | 2.5 W | 4–5 W | USB; only meaningful tracking cost |
| Wi-Fi AP (AX201, 5 GHz) | 1 W | 2–3 W | 802.11n single spatial stream is enough |
| iPhone (display+decode+ARKit+hands) | — | 3–5 W | HW decode, 60 fps cap, hands 30→15 Hz on thermal hint |
| **Total frame draw** | ~7 W | ~20–30 W | donor laptop battery (≥50 Wh) → 2–4 h; bigger pack → longer |

Mitigations built in:
- `NUOS_PKT_STAT` thermal flag: phone backs hands to 15 Hz + pauses passthrough; board drops bitrate 25% (floor 6 Mbps).
- Board: zram (8GB RAM), `--no-install-recommends` image, no desktop environment, core-pinned RT threads.
- 45/30 fps SESSION fallback allowed without renegotiation (frame pacing only).

Not yet implemented (v2 ideas): per-app render scaling, foveated encode,
Kiyo 30 fps mode (halves its draw), panel backlight fused-off via EC.
