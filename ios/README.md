# NuOS iOS viewer (NuosView)

iPhone 15 VR display + tracking pod for the NuOS x86 board over Wi-Fi.

## What this app does

- Discovers the board via Bonjour: `_nuos-xr._tcp` (TCP control on port 48012).
- Streams ARKit head pose (~60 Hz) and Vision hand tracking (30 Hz, 15 Hz when
  the board asks for thermal reduction) over UDP to the board's port 48011.
- Receives fragmented HEVC video (with XOR FEC) on UDP port 48010, reassembles
  frames, decodes with hardware VTDecompressionSession, and renders
  side-by-side stereo with rotation-only timewarp, barrel pre-distortion
  (adjustable k1/k2), and a dimmed ARKit passthrough backdrop.
- Probes `AVCaptureMultiCamSession` support (informational) and only
  advertises `NUOS_CAP_ULTRAWIDE` in the HELLO caps if the probe passes.
  ARKit owns the camera; no second capture session runs by default.

## Build

Requirements: macOS with Xcode 15+ (Swift 5.9, iOS 17 SDK) and
[xcodegen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).

```sh
cd ios/NuosView
xcodegen
open NuosView.xcodeproj
```

The repo's GitHub workflow (`.github/workflows/ios.yml`) builds an unsigned
IPA on every push touching `ios/**` and on `workflow_dispatch`.

## Sideload (unsigned IPA, free Apple ID)

1. Download the `NuosView-ipa` artifact from the workflow run (or build
   locally: archive with `CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO`,
   wrap the `.app` in a `Payload/` folder, zip it as `NuosView.ipa`).
2. Install with [Sideloadly](https://sideloadly.io) or
   [AltStore](https://altstore.io):
   - Plug the iPhone in, open Sideloadly, drop in `NuosView.ipa`.
   - Sign in with any free Apple ID (a personal "app-specific" Apple ID is
     recommended).
3. On the phone: Settings → General → VPN & Device Management → trust the
   developer profile.
4. Free-provisioned apps expire after 7 days; re-sideload to re-sign. AltStore
   refreshes automatically while on the same Wi-Fi as the signing Mac.

### What works on free provisioning

- Camera/ARKit (`NSCameraUsageDescription`) — yes, with the free profile.
- Local network + Bonjour (`NSLocalNetworkUsageDescription`,
  `NSBonjourServices = _nuos-xr._tcp`) — yes.
- A personal team signing identity — yes (7-day expiry, 3 app slots).
- Push notifications, background modes, custom entitlements — no.

## Notes / known risks

- Free-profile signing cannot be done on the CI (GitHub) runner; CI produces
  an *unsigned* IPA that must be signed on-device via Sideloadly/AltStore.
- `AVCaptureMultiCamSession.isMultiCamSupported` is generation-dependent;
  when the probe fails, `NUOS_CAP_ULTRAWIDE` is withheld and the board must
  not expect ultrawide passthrough.
- The phone's video receive port is fixed at 48010 per the NuOS protocol;
  running another app bound to it will make video fail.
