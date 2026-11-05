# Calibration Guide

## 1. Lens / display (per-user, in-app)
- Wear the headset, open **Diagnostics → Distortion**, adjust k1/k2 until
  straight lines look straight through the lenses (defaults 0.15/0.05).
- IPD: adjust the per-eye offset slider until the single image fuses.

## 2. Tracking (ARKit)
- Start tracking in a textured, evenly lit room. Avoid blank walls.
- Recenter = reset ARKit world origin from the home UI (maps to
  `ARSession` `setWorldOrigin` via the app's recenter button).
- If tracking shows "limited": ARKit reports the reason in Diagnostics
  (excessive motion / low light); no calibration needed, just conditions.

## 3. Kiyo ↔ ARKit fusion
- Fixed mount, lens roughly parallel to the phone's main camera.
- Intrinsics: defaults assume ~90° HFOV at 720p; calibrate with a checkerboard
  (`opencv` sample tools) and feed via `/etc/nuos/kiyo.yaml` (tracker `--calib`).
- Yaw-correction gain (`--yaw-gain`, default 0.05): raise if long-session yaw
  drifts; lower if the view "swims" during fast turns.
- Verify with the replay test: `nuos-tracker --replay clip.mp4`, check the
  fused-pose log for flat-topping drift.

## 4. Latency tuning
- Measure motion-to-photon feel: board STAT rtt + phone-side fps counters.
- If swimmy: lower bitrate before fps; verify jitter buffer stays at 2 frames.
- Encoder IDR interval: shorter = faster recovery, slightly more bitrate.

## 5. Wi-Fi
- 5 GHz AP (channel 36) is mandatory for 15 Mbps; 2.4 GHz will not hold 60 fps.
- Keep the phone ≥1 m from the AP antenna (avoid RSSI saturation), same room.
