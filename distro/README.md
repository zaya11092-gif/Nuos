# NuOS distro

Custom Debian 13 (trixie) x86_64 image for the NuOS DIY VR headset render
board (Lenovo IdeaPad Chromebook, MrChromebox UEFI, i3-1215U, Intel AX Wi-Fi,
UHD 64EU iGPU + QuickSync HEVC). The laptop panel stays off (`video=eDP-1:off`);
the board hosts a Wi-Fi AP for the iPhone and streams HEVC frames per
`proto/PROTOCOL.md`.

## Layout

| Path | Purpose |
|---|---|
| `build-image.sh` | Builds `nuos.img` (GPT: 256 MiB ESP + ext4 root) |
| `profile.env` | Suite/arch/UUID constants |
| `board/ideapad-chromebook.conf` | Board kernel cmdline + AP/power profile |
| `overlay/` | Files copied into the image rootfs (systemd units, AP config, scripts) |
| `tools/qemu-smoke.sh` | Boots the image under QEMU/OVMF, waits for `NUOS_BOOT_OK` |
| `tools/make-usb.sh` | `dd` the image to a USB drive |

## Build

Run on Linux (Debian/Ubuntu container or WSL2 as root — root ownership in the
rootfs matters):

```sh
sudo apt install mmdebstrap dosfstools e2fsprogs mtools fdisk
bash distro/build-image.sh --out nuos.img --size 4G --board ideapad-chromebook
# optional artifacts:
#   --bins platform/build/bin         -> /usr/local/bin in the image
#   --home-dir xr-home/export/dist    -> /opt/nuos/home in the image
```

The script is unprivileged (mmdebstrap, mtools, `mke2fs -d`) but preserves
correct file ownership only when run as root.

### Package name verification

The chroot hook in `build-image.sh` resolves exact trixie soname-suffixed
packages (ffmpeg 7.x: `libswscale9`/`libavutil60`; OpenCV 4.x:
`libopencv-core406t` etc.) via `apt-get -s` simulation at build time and
installs the first available candidate. If trixie renames them, adjust the
candidate lists in the hook.

## Flash and boot

```sh
sudo tools/make-usb.sh nuos.img /dev/sdX   # interactive confirmation
```

On the Chromebook (MrChromebox UEFI Full ROM installed): ESC/F7 at boot →
choose the USB entry. systemd-boot (installed on the ESP as
`\EFI\BOOT\BOOTX64.EFI`) boots the `nuos` entry after 3 s with:

```
root=PARTUUID=<fixed> ro quiet video=eDP-1:off console=ttyS0,115200 intel_iommu=on
i915.enable_guc=2 i915.enable_fbc=1
```

Serial console is on ttyS0,115200.

## First boot / AP credentials

`nuos-firstboot` runs once when `wlan0` exists: it generates SSID
`NuOS-XXXX` (from the interface MAC) and a random 12-char WPA2 PSK, writes
`/etc/hostapd/hostapd.conf` and the credentials to
`/var/lib/nuos/ap.conf` (mode 0600), then restarts hostapd/dnsmasq and
prints `SSID`/`PSK` on `/dev/console`. Recover them over serial or SSH:

```sh
cat /var/lib/nuos/ap.conf
```

SSH: `ssh root@10.66.0.1` (set a root password first via serial console, or
inject `/root/.ssh/authorized_keys` by mounting the image on another host).
The phone discovers the board via mDNS `_nuos-xr._tcp` (port 48012, TXT
`proto=1 board=nuos`) and connects per the session flow in `proto/PROTOCOL.md`.

## OpenXR / Monado status

The Debian `monado` package is installed as **scaffolding only** (OpenXR
loader + runtime JSON at `/usr/share/openxr/1/openxr_monado.json`). The
production runtime uses the custom driver built from
`platform/monado-nuos-driver`, which reads `/dev/shm/nuos_pose` (see
`proto/BRIDGE_API.md`); it replaces the stock monado device plugin. If the
distro package is absent at build time the image still boots; `nuos-home`
simply stays dormant until `/opt/nuos/home/bin/nuos-home` is provided via
`--home-dir`.

## Smoke test (no hardware)

```sh
sudo apt install qemu-system-x86 ovmf
tools/qemu-smoke.sh nuos.img
```

Boots with OVMF, no display, no NIC, serial to a log; passes when
`NUOS_BOOT_OK` appears within 180 s and fails on kernel panic. Hardware
daemons (`nuos-tracker`, `hostapd`, firstboot) skip or restart harmlessly
when their devices are absent.

## Building/running without Linux

`build-image.sh` targets Linux. On the Windows host, run it inside a root
Debian container (`docker run -v "$(pwd):/src" -w /src debian:trixie bash
distro/build-image.sh ...`) or WSL2.
