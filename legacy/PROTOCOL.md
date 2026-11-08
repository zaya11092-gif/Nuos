# NuOS Wire Protocol v1

All layouts live in [`nuos_proto.h`](nuos_proto.h) — that header is the source of
truth. This document describes flows, rates, and the rules each implementation
must follow. Swift mirror: `ios/NuosView/NuosView/Net/NuosProto.swift`.

## Endpoints

| Channel | Transport | Direction | Port | Rate |
|---|---|---|---|---|
| Video (HEVC frags + FEC) | UDP | board → phone | 48010 | 60 fps, ~23 frags/frame @ 15 Mbps |
| Pose | UDP | phone → board | 48011 | ~60 Hz (ARKit camera rate) |
| Hands | UDP | phone → board | 48011 | 30 Hz (thermal-throttled to 15 Hz) |
| Stats | UDP | both, over their own send sockets | 48011 (phone→board) | 1 Hz |
| Control | TCP | phone → board | 48012 | on demand |
| Tracker state | UDP localhost | tracker → xr-home | 48100 | 10 Hz |
| Hands fan-out | UDP localhost | tracker → handbus | 48101 | as received |

Board Wi-Fi AP: `10.66.0.1/24` on `wlan0`, DHCP via dnsmasq, SSID `NuOS-XXXX`.
mDNS advertisement: `_nuos-xr._tcp` on port 48012 (TXT: `board=nuos`, `proto=1`).

## Session flow

```
phone                                board
  │ mDNS browse _nuos-xr._tcp           │
  │ ──── TCP connect ──────────────────▶│ :48012
  │ ◀─── HELLO(board caps) ─────────────│  (first frame from both sides)
  │ ──── HELLO(phone caps) ────────────▶│
  │ ──── SESSION(fps, res, kbps) ──────▶│
  │ ◀─── video frags @48010 ────────────│  (to phone's TCP peer IP)
  │ ──── pose+hands @48011 ────────────▶│
  │ ◀─── PONG / STAT ───────────────────│  (PING from either side)
  │ ──── STOP (or TCP close) ──────────▶│
```

- The phone's video-receive port is fixed at 48010; the board streams to the
  IP the TCP connection came from (NAT-friendly).
- HELLO caps negotiation: intersection of both sides. `NUOS_CAP_ULTRAWIDE`
  is set by the phone only if its runtime camera probe proved simultaneous
  capture support (see docs/ios-camera-constraints.md).

## Video framing rules

1. One HEVC access unit per `frame_seq`; split into `frag_count` fragments of
   at most `NUOS_VIDEO_PAYLOAD_MAX (1444) bytes, same `frag_count` in all.
2. `nuos_video_t` fields are identical in every fragment of a frame, except
   `frag_idx` and `payload_len` (last fragment may be short).
3. Sender must emit fragments back-to-back; no pacing within a frame.
4. FEC: after fragments `g*NUOS_FEC_GROUP .. (g+1)*NUOS_FEC_GROUP-1` of a
   frame, send one `NUOS_PKT_FEC` whose payload is the XOR of those fragment
   payloads (zero-padded to the longest). Receiver reconstructs only if
   exactly one fragment of the group is missing.
5. Receiver keeps a 2-frame jitter buffer (≈33 ms @60 fps). The *newest
   fully-assembled* frame is presented each vsync; older ones are dropped.
6. `render_pos/render_quat` = board pose used at compose time. The phone
   computes `delta = latest_pose ∘ render_pose⁻¹` and applies a rotation-only
   timewarp (see Renderer). Never warp translation — streaming latency makes
   positional warp error-prone.

## Pose rules

- Phone sends at ARKit camera update rate (~60 Hz on iPhone 15), tracking
  state included; board extrapolates with `vel`/`ang_vel` by up to 1.5 frames
  when composing (pose prediction).
- Board time `t_mono_ns` is `CLOCK_MONOTONIC` on Linux; phone uses
  `mach_continuous_time`. Never compared directly — RTT comes from PING.

## Hands

- 21 joints follow Vision's `VNHumanHandPoseObservation.JointName` ordering:
  wrist first, then thumb(4), index(4), middle(4), ring(4), little(4).
  A canonical ordering table is in `NuosProto.swift` and must be mirrored
  exactly on the board (`platform/src/nuos-handbus`).
- Coordinates: x,y normalized to the camera image (y down, top-left origin);
  z is a relative depth proxy (smaller = closer). Good enough for UI pointing
  and pinch gestures — the board never assumes metric scale.
- Pinch = distance(wrist, index-tip) below a normalized threshold; XR home
  applies hysteresis (see xr-home/scripts/hand_pointer.gd).

## Clocks, rates, and thermal

- Either side may send `NUOS_CTL_STAT_REQ`; both sides emit `NUOS_PKT_STAT`
  at ~1 Hz on their telemetry sockets.
- `flags bit0` in STAT = "please reduce": phone backs hands off to 15 Hz and
  pauses passthrough encode; board drops bitrate 25% (floor 6 Mbps).
- Thermal state changes may also adjust SESSION: fps 60 → 45 → 30 is
  allowed without renegotiation (frame pacing only).

## Versioning

`NUOS_PROTO_VERSION` bumps on any layout change. Mismatched HELLO versions
→ both sides drop the session with a log line; no fallback wire format in v1.
