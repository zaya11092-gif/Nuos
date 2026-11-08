# NuOS Bridge API (board-internal contracts)

The board runs several processes that exchange data through shared memory and
localhost UDP. Everything here is defined by headers in
`platform/include/` and `proto/nuos_proto.h` — do not fork the definitions.

```
phone ──UDP 48011──▶ nuos-tracker ──seqlock──▶ /dev/shm/nuos_pose ──▶ monado driver
                        │  │                      /dev/shm/nuos_hands ──▶ nuos-handbus ◀─┐
                        │  └─UDP 48101 (hands fwd)─────────────────────────────────────┘
                        └─UDP 48100 (state, 10 Hz)──▶ xr-home dashboard (via bridge)

xr-home (Godot) ──GDExtension NuosBridge.push_frame()──▶ /dev/shm/nuos_frames
nuos-stream ◀──reads ring──── /dev/shm/nuos_frames ──▶ VAAPI HEVC ──UDP 48010──▶ phone
phone ◀──TCP 48012 control (HELLO/SESSION/STOP/PING)──▶ nuos-stream control server
```

## Ownership

| Object | Creator | Writers | Readers |
|---|---|---|---|
| `/dev/shm/nuos_pose` | nuos-tracker | nuos-tracker | monado driver, NuosBridge, stream (render pose) |
| `/dev/shm/nuos_hands` | nuos-handbus | nuos-handbus | NuosBridge |
| `/dev/shm/nuos_frames` | NuosBridge (first open) | NuosBridge | nuos-stream |

## NuosBridge GDExtension (`platform/gdext-nuos-bridge/`)

A Godot 4 GDExtension, registered class `NuosBridge` (extends `Node`).

```gdscript
var bridge := NuosBridge.new()
add_child(bridge)   # connects/creates shm in _ready(), starts 10 Hz poll timer

bridge.get_head_pose() -> Dictionary
    { "pos": Vector3, "quat": Quaternion, "t_ns": int,
      "tracking": int, "kiyo_ok": bool, "arkit_ok": bool }
    (zero values if shm absent — flat/debug mode must keep working)

bridge.get_hands() -> Dictionary
    { "present": int,            # bit0 left, bit1 right
      "left":  PackedVector3Array(21),   # normalized image coords
      "right": PackedVector3Array(21),
      "t_ns": int }

bridge.get_stats() -> Dictionary
    { "fps": float, "bitrate_kbps": int, "loss_ppm": int,
      "rtt_ms": float, "battery_pct": int, "temp_c": float }

bridge.push_frame(image: Image) -> bool
    # image.format = FORMAT_RGBA8 (converted to BGRA8 in-place by the bridge)
    # size must be <= NUOS_RING_MAX_W x NUOS_RING_MAX_H (2560x800)
    # returns false (and logs once) if shm not yet open or size invalid
```

Suggested home render loop: `_process` → render scene to a `SubViewport`
(SBS stereo, per-eye `width x height` from SESSION), `await RenderingServer.
frame_post_draw`, `bridge.push_frame(viewport.get_texture().get_image())`.

## Localhost state packet (tracker → dashboard, UDP 48100, 10 Hz)

Payload = `nuos_pose_t` from `proto/nuos_proto.h` (t_mono_ns = board clock),
sent once per tick; the bridge's `get_stats()` merge comes from `NUOS_PKT_STAT`
monitoring inside the tracker. Consumers must tolerate packet loss (it's UDP).

## Hands fan-out (UDP 48101)

nuos-tracker re-emits any `NUOS_PKT_HANDS` datagram received on 48011 verbatim
to `127.0.0.1:48101`; nuos-handbus listens there and writes `/dev/shm/nuos_hands`.
Keeping the tracker as the single owner of the 48011 socket avoids two daemons
fighting over the same UDP port.
