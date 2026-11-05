# NuOS XR Home (Godot 4.3)

The XR "home" launcher for the NuOS DIY headset. It renders the launcher UI
into a SubViewport (per-eye 1280x720) and streams frames to the phone through
the `NuosBridge` GDExtension (`push_frame()` -> `/dev/shm/nuos_frames` -> VAAPI
HEVC -> UDP 48010, see `proto/BRIDGE_API.md`).

It runs in two modes:

- **Flat/desktop (dev):** if OpenXR is not available the app falls back to a
  normal 1280x720 window with a slowly orbiting camera and a simulated
  pointer (gaze + left mouse click). The bridge GDExtension may be absent;
  all bridge data degrades to safe dummy values.
- **XR (board):** OpenXR via Monado. Head pose comes from the bridge / OpenXR,
  hands from the bridge (21 joints, Vision order), and the frame loop pushes
  SBS stereo frames to the phone.

## Run (flat, dev)

```sh
godot --path xr-home
```

## Run on the board (Monado XR)

```sh
# Point OpenXR at the Monado runtime, then launch. Example:
export XR_RUNTIME_JSON=/usr/share/openxr/1.0/monado.json  # adjust to your distro
./export/nuos-home.x86_64
```

## NuosBridge GDExtension placement

Build `platform/gdext-nuos-bridge` and copy its output next to the exported
binary (or into `xr-home/` while running from source):

```
<app dir>/
  nuos_bridge.gdextension      # the .gdextension file from the bridge build
  libnuos-bridge.so            # built shared library
```

Godot loads registered GDExtension classes from the executable's directory or
the project directory. If the class `NuosBridge` is not registered, xr-home
prints a notice and runs on dummy data — nothing crashes.

## apps.json schema

`/etc/nuos/apps.json` (optional). JSON array of objects:

```json
[
  {
    "id": "settings",          // required, unique string
    "title": "Settings",       // required, card label
    "subtitle": "Dashboard",   // optional, small card label
    "builtin": "dashboard",    // optional: "dashboard" opens the live stats
                               // panel; anything else (or missing) spawns a
                               // stub panel. External exec spawning is a stub
                               // in v1.
    "icon": "res://icon.svg"   // optional, reserved for future use
  }
]
```

If the file is missing, malformed, or empty, the built-in defaults are used
(Settings, Files-lite, TestPattern, About).

## Known v1 limitations

- Passthrough is composited on the PHONE (see `proto/PROTOCOL.md`); the
  in-app "passthrough backdrop" is only a dark gradient environment used in
  flat mode / as VR fallback.
- Stereo SBS is produced by rendering the single SubViewport twice per pushed
  frame (per-eye frustum offset approximation), not by Godot's XR swapchain.
- Hand rays map normalized camera coords 1:1 onto screen UV; camera-to-head
  extrinsic alignment is pending.
