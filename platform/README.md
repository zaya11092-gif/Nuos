# NuOS Platform (board-side daemons)

Linux (Debian 13, x86_64) daemons for the NuOS DIY VR headset. Contracts come
from `proto/nuos_proto.h`, `proto/PROTOCOL.md`, `proto/BRIDGE_API.md`,
`platform/include/nuos_shm.h`, `platform/include/nuos_ring.h` — read those
first; they are the single source of truth.

## Components

| Binary | Role |
|---|---|
| `nuos-tracker` | Owns UDP 48011 (ARKit pose/hands/stat from phone), fuses with Kiyo mono VO (KLT + essential matrix, yaw-drift correction; translation is scale-ambiguous and used as direction only), writes `/dev/shm/nuos_pose` at ~90 Hz, forwards hands verbatim to 48101, state packets to 48100 at 10 Hz. |
| `nuos-stream` | TCP control :48012 (HELLO/SESSION/STOP/PING/STAT_REQ), reads `/dev/shm/nuos_frames`, swscale BGRA→NV12, VAAPI HEVC low-power encode, fragments ≤1436 B with `nuos_video_t` headers + XOR FEC, streams UDP to the phone's TCP peer IP :48010. |
| `nuos-handbus` | Listens 127.0.0.1:48101, writes `/dev/shm/nuos_hands` (seqlock). |
| `gdext-nuos-bridge/` | Godot 4 GDExtension `NuosBridge` (see its README). Creates `/dev/shm/nuos_frames` on first `push_frame`. |
| `monado-nuos-driver/` | Best-effort Monado null-HMD driver injecting `/dev/shm/nuos_pose` each frame (see its `build-monado.sh`). |

## Build

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libopencv-dev libva-dev libdrm-dev libswscale-dev libavutil-dev
cd platform
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Run order

```sh
./build/nuos-handbus &
./build/nuos-tracker --device /dev/video0 --core 2 --rt   # or --replay clip.mp4
# then start xr-home in Godot (loads NuosBridge, pushes frames)
./build/nuos-stream --core 3          # waits for ring; phone connects to :48012
# optionally: platform/monado-nuos-driver/build-monado.sh
```

`nuos-tracker` degrades gracefully: no Kiyo camera → pure ARKit passthrough.

## Ports

| Port | Transport | Direction | Used by |
|---|---|---|---|
| 48010 | UDP | board → phone | nuos-stream video fragments + FEC |
| 48011 | UDP | phone → board | nuos-tracker (pose / hands / stat) — single socket owner |
| 48012 | TCP | phone → board | nuos-stream control server |
| 48100 | UDP localhost | tracker → dashboard | fused state, 10 Hz (`nuos_pose_t`) |
| 48101 | UDP localhost | tracker → handbus | hands verbatim |

## Shared memory

| Object | Creator | Writers | Readers |
|---|---|---|---|
| `/dev/shm/nuos_pose` | nuos-tracker | nuos-tracker | Monado driver, NuosBridge, nuos-stream |
| `/dev/shm/nuos_hands` | nuos-handbus | nuos-handbus | NuosBridge |
| `/dev/shm/nuos_frames` | NuosBridge (first push_frame) | NuosBridge | nuos-stream |

All reads/writes use the seqlock helpers in `nuos_shm.h` / `nuos_ring.h`.

## CLI

- `nuos-tracker --device /dev/video0 | --replay file`, `--core N`
  (pthread_setaffinity_np), `--rt` (SCHED_FIFO, needs root or setcap).
- `nuos-stream --core N --rt`.

## Known notes / risks

- Kiyo VO is **scale-ambiguous** (mono camera): rotation is trusted, translation
  only as a unit direction for yaw drift correction — never metric position.
- VAAPI HEVC low-power (`VAEntrypointEncSliceLP`) requires an Intel iGPU with
  the iHD driver; the stream idles (with a log) if init fails.
- `nuos-stream`'s control-server STAT over TCP uses a local reserved marker;
  the canonical STAT path is the UDP telemetry socket per PROTOCOL.md.
