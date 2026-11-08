# NuOS v2

VR headset platform: **iPhone (Unity/CardboardXR)** as the headset — local
rendering, ARKit 6DoF, hand tracking, room setup — connected over the
**PhoneVR link protocol** to a **Linux board** that runs **Steam + desktop
apps** and streams their windows as floating 3D windows above the app dock.

```
iPhone 15 (Unity 6, CardboardXR)          Board (Ubuntu, i3-1215U)
├─ ARKit 6DoF pose + room setup           ├─ nuospvr-server (PhoneVR protocol)
├─ Hand tracking (MediaPipe, on-phone)    ├─ X11 window capture → QuickSync HEVC
├─ Home env + dock (app library)          ├─ Steam + desktop apps
└─ FloatingWindow (board app streams)  ◀━━┛ UDP 33333 / TCP 33333·15243·51423
```

- `unity/` — Unity 6 project (CardboardXR-derived) + `PVR/` (link, decoder,
  floating window, bootstrap)
- `server/` — `nuospvr-server` (C++): protocol, X11 window capture,
  hevc_vaapi encoder
- `setup/nuos2-setup.sh` — board provisioning (Steam, X11 session, VAAPI)
- `docs/protocol-v2.md` — wire reference
- `legacy/` — v1 artifacts and hard-won notes

## Phone build

1. Install **Unity 6000.6.8f1** (free Personal) on a Windows PC.
2. Open `unity/` → File → Build Settings → iOS → Build → produces the Xcode
   project (e.g. `unity/build/Unity-iPhone/`).
3. Commit that Xcode project (it's picked up by CI automatically).

CI (`.github/workflows/ios.yml`) compiles the committed Xcode project on macOS
runners into an unsigned IPA and attaches it to releases on `v*` tags.

## Board setup

```
scp -r setup server a@<board>:~/nuos2/   # then:
sudo bash ~/nuos2/setup/nuos2-setup.sh   # Steam, X11 session, VAAPI, deps
cd ~/nuos2/server && cmake -B build -S . -G Ninja && ninja -C build
DISPLAY=:1 ./build/nuospvr-server &      # run inside the desktop session
```

Verify without a phone: `tools/fake-phone.py <board_ip>` performs the full
handshake, lists windows, streams one, and dumps decodable HEVC.

## Status

- Board link **proven end-to-end** (see `legacy/evidence/` + git history):
  handshake, window capture, QuickSync HEVC, decode-verified frames.
- Phone app: PVRLink/PVRDecoder/FloatingWindow implemented; awaiting first
  Unity build + on-device test.
