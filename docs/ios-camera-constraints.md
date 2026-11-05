# iOS Camera Constraints (iPhone 15)

## Facts the design is built around

- iPhone 15 (base) has **main (wide) + ultrawide** rear cameras, no telephoto.
  Display is 60 Hz fixed (no ProMotion).
- **ARKit owns the camera pipeline.** No `AVCaptureSession` /
  `AVCaptureMultiCamSession` can run concurrently with an `ARSession`.
  → SLAM, passthrough frames, and hand-tracking input all come from ARKit's
  main-camera `ARFrame.capturedImage`.
- **Wide + ultrawide simultaneous capture is generally NOT a supported
  `AVCaptureMultiCamSession` configuration** on this class of device
  (supported configs are typically one back camera + front). Whether a given
  iOS version/unit allows it can only be determined at runtime.

## The chosen architecture (ARKit-first + probe)

1. Default path (always works): main camera → ARKit VIO (pose @60 Hz) +
   passthrough frames + Vision hand tracking.
2. `UltraWideProbe` (Diagnostics screen) enumerates
   `AVCaptureMultiCamSession.isMultiCamSupported` and each device's
   `supportedMultiCamSessionConfigurations` and displays them.
3. Only if the probe passes does the app set `NUOS_CAP_ULTRAWIDE` and enable
   the experimental multi-cam path (off by default, refuses while ARSession
   is active).

## Probe result template (fill in from your unit)

```
Device: iPhone 15, iOS __.__
isMultiCamSupported:            yes / no
builtInWideAngleCamera configs:   (list from Diagnostics)
builtInUltraWideCamera configs:   (list from Diagnostics)
builtInDualWideCamera configs:    (list from Diagnostics)
=> ultrawide passthrough enabled: yes / no
```

## Consequences

- Passthrough FOV = main camera FOV (~69°), narrower than the ultrawide's
  120°. If the probe ever passes, passthrough/hands can migrate to the
  ultrawide with wider FOV.
- Hand tracking is 2D-normalized (no metric scale) — good enough for UI
  pointing + pinch; documented in `proto/PROTOCOL.md`.
