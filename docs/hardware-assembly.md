# Hardware Assembly

## Board prep (Lenovo IdeaPad Chromebook, MrChromebox UEFI)

1. Firmware: MrChromebox **full ROM** UEFI must be installed (write-protect
   screw removed/disabled first). Recovery: flash from USB via `setvars`/boot
   stub — see mrchromebox.tech docs.
2. The board runs headless; keep the battery connector, keyboard/trackpad can
   be unplugged (saves ~0.5 W). Power button: short the header pads or keep a
   small tact switch wired to the frame for hard resets.
3. Charging: use the laptop's own DC-in barrel/USB-C with its original
   charger circuit. Do not fast-charge while under full load without thermal
   padding.

## Flashing & first boot

```
wsl:  distro/build-image.sh --out nuos.img --board ideapad-chromebook \
        [--bins build/bin] [--home-dir xr-home/export]
win:  tools/make-usb.sh  (run under Linux/WSL; pick the USB device carefully)
```
Boot → F12/ESC → MrChromebox boot menu → USB. First boot: `nuos-firstboot`
generates SSID `NuOS-XXXX` + PSK, prints them on serial console (open the
bottom panel or use a serial dongle; alternatively SSH later via ethernet).
They persist in `/var/lib/nuos/ap.conf`.

## Mounting

- **iPhone cradle**: lens-to-screen optics (Cardboard-style), phone screen is
  the VR display. Keep the back cameras unobstructed; the phone must be
  rigidly fixed to the frame — any flex breaks VIO.
- **Kiyo**: forward-facing, near the phone's main camera line-of-sight
  (small baseline offset is fine, 2–10 cm), rigid mount, USB to the board.
- **Board + battery**: center of mass near the back of the frame; heatsink
  fan exhaust must not blow into the optics.

## Calibration (see also docs/calibration.md)

1. Barrel distortion k1/k2 to match lens optics (Diagnostics sliders).
2. ARKit↔Kiyo extrinsics: place a static scene, run tracker with
   `--replay`, tune the yaw-correction gain until drift curve flattens.
3. Play space: ARKit world origin = wherever tracking starts; home recenter
   button resets it.
