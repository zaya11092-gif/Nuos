# NuOS Bridge GDExtension

Godot 4 GDExtension registering `NuosBridge` (extends `Node`), the API from
`proto/BRIDGE_API.md`:

- `get_head_pose()` — `/dev/shm/nuos_pose` seqlock read; zero values when the
  shm is absent (flat/debug mode keeps working).
- `get_hands()` — `/dev/shm/nuos_hands` seqlock read; 21 joints per hand in
  Vision ordering, normalized image coords.
- `get_stats()` — board telemetry summary (v1: zeroed placeholder until the
  STAT merge from the tracker state packets lands; see Risks in the report).
- `push_frame(image)` — creates `/dev/shm/nuos_frames` on first call (ring
  creator per BRIDGE_API.md), converts RGBA8 → BGRA8, writes the newest slot
  via per-slot seqlock. Returns false (logs once) if shm cannot be opened or
  the image exceeds 2560x800.

## Build

```sh
git clone https://github.com/godotengine/godot-cpp
cd godot-cpp && git submodule update --init --recursive && scons platform=linux target=template_debug
cd ../gdext-nuos-bridge
scons platform=linux target=template_debug GODOT_CPP=$(pwd)/../godot-cpp
```

Output: `bin/libnuos_bridge.linux.template_debug.x86_64.so`. Copy `nuos_bridge.gdextension`
next to `bin/` inside the Godot project's `extensions/` directory.

## Usage

```gdscript
var bridge := NuosBridge.new()
add_child(bridge)
var pose = bridge.get_head_pose()
bridge.push_frame($SubViewport.get_texture().get_image())
```
