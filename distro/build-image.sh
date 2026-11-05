#!/usr/bin/env bash
# NuOS image builder.
#
# Builds a raw GPT disk image (ESP + ext4 root) for the NuOS DIY VR headset
# render board. Runs unprivileged with mmdebstrap; populates the filesystems
# without loop mounts (mtools for FAT, mke2fs -d for ext4), so it works in a
# container/CI and on a Linux host alike. Best results run as root (Debian
# container, WSL2 as root): ownership in the rootfs is then preserved.
#
# Usage:
#   build-image.sh [--out nuos.img] [--size 4G] [--board ideapad-chromebook]
#                  [--bins DIR] [--home-dir DIR]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="nuos.img"
SIZE="4G"
BOARD="ideapad-chromebook"
BINS_DIR=""
HOME_DIR=""
ISO_OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --out)      OUT="$2"; shift 2 ;;
        --size)     SIZE="$2"; shift 2 ;;
        --board)    BOARD="$2"; shift 2 ;;
        --bins)     BINS_DIR="$2"; shift 2 ;;
        --home-dir) HOME_DIR="$2"; shift 2 ;;
        --iso)      ISO_OUT="nuos.iso"; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# --- profile + board -------------------------------------------------------
# shellcheck source=profile.env
source "$SCRIPT_DIR/profile.env"
# shellcheck source=board/ideapad-chromebook.conf
source "$SCRIPT_DIR/board/$BOARD.conf"

# --- tool checks -----------------------------------------------------------
EXTRA_TOOLS=""; [ -n "$ISO_OUT" ] && EXTRA_TOOLS="cpio xorriso"
for tool in mmdebstrap sfdisk mkfs.vfat mkfs.ext4 mcopy dd fallocate $EXTRA_TOOLS; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: required tool '$tool' not found." >&2
        echo "  Debian/Ubuntu: apt install mmdebstrap fdisk dosfstools e2fsprogs mtools" >&2
        exit 1
    }
done

WORK="$(mktemp -d /tmp/nuos-build.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
ROOTFS="$WORK/rootfs"
ESP="$WORK/esp.img"

# --- chroot customization hook (executed inside the rootfs by mmdebstrap) ---
# Resolves exact trixie package names for OpenCV / ffmpeg runtime libs with
# apt-cache simulation, installs them plus the monado scaffolding package.
cat > "$WORK/nuos-extras.sh" <<'HOOK'
#!/bin/sh
set -e
export DEBIAN_FRONTEND=noninteractive

pick() {
    for c in "$@"; do
        if apt-get -s install --no-install-recommends "$c" >/dev/null 2>&1; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}

# trixie ships ffmpeg 7.x -> libavutil60/libswscale9; fall back for safety.
FF=$(pick libswscale9 libswscale8) || FF=
AV=$(pick libavutil60 libavutil59) || AV=
# trixie OpenCV 4.x runtime; soname-suffixed core/imgproc packages.
OC=$(pick libopencv-core406t libopencv-core410 libopencv-core412 libopencv-core413) || OC=
OI=$(pick libopencv-imgproc406t libopencv-imgproc410 libopencv-imgproc412 libopencv-imgproc413) || OI=

PKGS="iproute2"
for p in "$FF" "$AV" "$OC" "$OI"; do
    [ -n "$p" ] && PKGS="$PKGS $p"
done
echo "nuos: installing extra packages:$PKGS"
apt-get install -y --no-install-recommends $PKGS

# monado OpenXR runtime: scaffolding only; the custom NuOS driver lives in
# platform/monado-nuos-driver and replaces the stock device plugin at runtime.
if apt-get -s install --no-install-recommends monado >/dev/null 2>&1; then
    apt-get install -y --no-install-recommends monado
    mkdir -p /usr/share/openxr/1
    if [ -f /usr/share/openxr/1/openxr_monado.json ] || \
       find /usr/share -name 'openxr_monado.json' -print -quit | grep -q .; then
        echo "nuos: monado OpenXR runtime present"
    fi
else
    echo "nuos: monado package not available; OpenXR runtime must come from platform/monado-nuos-driver build"
fi

# systemd-networkd-wait-online would block boot when only wlan0 exists and
# no AP link is up yet; boot-ok deliberately orders on networkd, not this.
if [ -e /lib/systemd/system/systemd-networkd-wait-online.service ]; then
    rm -f /etc/systemd/system/systemd-networkd-wait-online.service
    ln -s /dev/null /etc/systemd/system/systemd-networkd-wait-online.service
fi
HOOK
chmod +x "$WORK/nuos-extras.sh"

# --- package list ----------------------------------------------------------
PACKAGES=(
        systemd-boot
        linux-image-amd64
    firmware-linux
    firmware-linux-nonfree
    intel-microcode
    intel-media-va-driver-non-free
    lightdm
    lightdm-gtk-greeter
    xfce4
    xfce4-terminal
    sudo
    fonts-dejavu-core
    dbus-x11
    hostapd
    dnsmasq
    avahi-daemon
    avahi-utils
    zram-tools
    openssh-server
    ca-certificates
    curl
    vim-tiny
    systemd
    systemd-sysv
    udev
    kmod
    e2fsprogs
    libva2
    libva-drm2
    libva-x11-2
)

echo "==> mmdebstrap: building $DEBIAN_SUITE/$ARCH rootfs"
if [ "$(id -u)" -ne 0 ]; then
    echo "WARNING: not root; rootfs file ownership will be mapped to uid $(id -u)." >&2
    echo "         Build inside a root container or WSL2-as-root for a correct image." >&2
fi
mmdebstrap \
    --mode=unshare \
    --variant=apt \
    --architectures="$ARCH" \
    --components="$DEBIAN_COMPONENTS" \
    --aptopt='Apt::Install-Recommends "false";' \
    --customize="$WORK/nuos-extras.sh" \
    --include="$(IFS=,; echo "${PACKAGES[*]}")" \
    "$DEBIAN_SUITE" - > "$WORK/rootfs.tar"
mkdir -p "$ROOTFS"
tar -xpf "$WORK/rootfs.tar" -C "$ROOTFS"

# --- overlay / user artifacts ---------------------------------------------
echo "==> copying overlay"
cp -a "$SCRIPT_DIR/overlay/." "$ROOTFS/"

# --- GUI: XFCE autologin on the board panel ---------------------------------
echo "==> configuring GUI (XFCE autologin)"
NUOS_PW_HASH=$(openssl passwd -6 nuos)
mkdir -p "$ROOTFS/home/nuos" "$ROOTFS/etc/lightdm/lightdm.conf.d"          "$ROOTFS/etc/sudoers.d"
printf 'nuos:x:1000:1000:NuOS User,,,:/home/nuos:/bin/bash
' >> "$ROOTFS/etc/passwd"
printf 'nuos:x:1000:
' >> "$ROOTFS/etc/group"
printf "nuos:$NUOS_PW_HASH:19000:0:99999:7:::
" >> "$ROOTFS/etc/shadow"
printf 'nuos:x:1000:
' >> "$ROOTFS/etc/gshadow"
chown -R 1000:1000 "$ROOTFS/home/nuos"
printf 'nuos ALL=(ALL) NOPASSWD: ALL
' > "$ROOTFS/etc/sudoers.d/nuos"
chmod 440 "$ROOTFS/etc/sudoers.d/nuos"
cat > "$ROOTFS/etc/lightdm/lightdm.conf.d/50-nuos-autologin.conf" <<LDMCONF
[Seat:*]
autologin-user=nuos
autologin-user-timeout=0
autologin-session=xfce
user-session=xfce
LDMCONF
# PAM group so autologin works without a password prompt
mkdir -p "$ROOTFS/etc/systemd/system/getty@tty1.service.d"
rm -f "$ROOTFS/etc/systemd/system/default.target"
ln -s /lib/systemd/system/graphical.target "$ROOTFS/etc/systemd/system/default.target"
ln -s /lib/systemd/system/lightdm.service     "$ROOTFS/etc/systemd/system/display-manager.service"

if [ -n "$BINS_DIR" ]; then
    echo "==> copying binaries from $BINS_DIR"
    mkdir -p "$ROOTFS/usr/local/bin"
    cp -a "$BINS_DIR/." "$ROOTFS/usr/local/bin/"
fi

if [ -n "$HOME_DIR" ]; then
    echo "==> copying nuos-home payload from $HOME_DIR"
    mkdir -p "$ROOTFS/opt/nuos/home"
    cp -a "$HOME_DIR/." "$ROOTFS/opt/nuos/home/"
fi

# --- identity ---------------------------------------------------------------
printf '%s\n' "$NUOS_HOSTNAME" > "$ROOTFS/etc/hostname"
cat > "$ROOTFS/etc/hosts" <<HOSTS
127.0.0.1   localhost
127.0.1.1   $NUOS_HOSTNAME
HOSTS

# machine-id is regenerated on first boot (avoids cloned-image duplicates).
rm -f "$ROOTFS/etc/machine-id"
: > "$ROOTFS/etc/machine-id"

# --- fstab (PARTUUID from profile.env) --------------------------------------
cat > "$ROOTFS/etc/fstab" <<FSTAB
# /etc/fstab: static filesystem table (generated by distro/build-image.sh)
PARTUUID=$NUOS_ROOT_PARTUUID /         ext4  defaults,noatime          0 1
PARTUUID=$NUOS_ESP_PARTUUID  /boot/efi vfat  defaults,noatime,umask=0077 0 2
tmpfs /tmp tmpfs defaults,nosuid        0 0
FSTAB

# --- offline systemd enablement (pure symlinks, no chroot) ------------------
echo "==> enabling systemd units (offline symlinks)"
W="$ROOTFS/etc/systemd/system"
ln_fn() { # ln_fn <target> <linkpath>
    mkdir -p "$(dirname "$2")"
    ln -sfn "$1" "$2"
}

# nuos.target under multi-user
ln_fn /etc/systemd/system/nuos.target "$W/multi-user.target.wants/nuos.target"
# board services under nuos.target
for svc in nuos-boot-ok nuos-tracker nuos-stream nuos-handbus nuos-home nuos-firstboot; do
    ln_fn "../$svc.service" "$W/nuos.target.wants/$svc.service"
done
if [ "${POWER_TUNE:-false}" = "true" ]; then
    ln_fn ../nuos-power-tune.service "$W/nuos.target.wants/nuos-power-tune.service"
fi
# distro services
ln_fn /lib/systemd/system/systemd-networkd.service "$W/multi-user.target.wants/systemd-networkd.service"
ln_fn /lib/systemd/system/avahi-daemon.service     "$W/multi-user.target.wants/avahi-daemon.service"
ln_fn /lib/systemd/system/dnsmasq.service          "$W/multi-user.target.wants/dnsmasq.service"
ln_fn /lib/systemd/system/hostapd.service          "$W/multi-user.target.wants/hostapd.service"
ln_fn /lib/systemd/system/ssh.service              "$W/multi-user.target.wants/ssh.service"

# --- kernel cmdline ----------------------------------------------------------
KVER="$(ls "$ROOTFS/boot" | grep -E '^vmlinuz-' | sort -V | tail -1)"
KVER="${KVER#vmlinuz-}"
[ -n "$KVER" ] || { echo "ERROR: no kernel found in rootfs/boot" >&2; exit 1; }
echo "==> kernel version: $KVER"

BASE_CMDLINE="$(sed 's/%ROOT_PARTUUID%/'"$NUOS_ROOT_PARTUUID"'/g' \
    "$ROOTFS/etc/kernel/cmdline")"
FULL_CMDLINE="$BASE_CMDLINE ${CMDLINE_EXTRA:-}"

# --- partition table ----------------------------------------------------------
# --- ISO (bootable installer medium) --------------------------------------
if [ -n "$ISO_OUT" ]; then
    echo "==> building installer ISO $ISO_OUT"
    cp -a "$SCRIPT_DIR/overlay-installer/." "$ROOTFS/"
    mkdir -p "$ROOTFS/etc/systemd/system/multi-user.target.wants"
    ln -sf ../nuos-installer.service         "$ROOTFS/etc/systemd/system/multi-user.target.wants/nuos-installer.service"
    touch "$ROOTFS/.live-installer"
    # live system must not try to mount the raw-image fstab PARTUUIDs
    printf '# NuOS live installer - no fstab entries
' > "$ROOTFS/etc/fstab"
    # initramfs needs an /init or the kernel panics after unpack (no root= support)
    cat > "$ROOTFS/init" <<'INITEOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /run
mount -t tmpfs tmpfs /run
exec /sbin/init
INITEOF
    chmod +x "$ROOTFS/init"
    KVER=$(cd "$ROOTFS/boot" && ls vmlinuz-* 2>/dev/null | head -1 | cut -d- -f2-)
    [ -n "$KVER" ] || { echo "no kernel found for ISO" >&2; exit 1; }
    echo "==> packing live initramfs (large, gzip -1)"
    (cd "$ROOTFS" && find . -xdev -print0 | cpio --null -o -H newc --quiet | gzip -1)         > "$WORK/initrd-full.img"
    ls -lh "$WORK/initrd-full.img" >&2

    ISO_ESP="$WORK/esp-iso.img"
    truncate -s 2G "$ISO_ESP"
    mkfs.vfat -F 32 -n NUOSISO "$ISO_ESP"
    mmd -i "$ISO_ESP" ::EFI ::EFI/BOOT ::loader ::loader/entries
    SB_EFI="$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
    [ -f "$SB_EFI" ] || SB_EFI="$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootx64.efi.signed"
    mcopy -i "$ISO_ESP" "$SB_EFI" ::EFI/BOOT/BOOTX64.EFI
    mcopy -i "$ISO_ESP" "$ROOTFS/boot/vmlinuz-$KVER" ::vmlinuz
    mcopy -i "$ISO_ESP" "$WORK/initrd-full.img" ::initrd.img
    printf 'timeout 5
default nuos-live
' > "$WORK/loader.conf"
    mcopy -i "$ISO_ESP" "$WORK/loader.conf" ::loader/loader.conf
    cat > "$WORK/nuos-live.conf" <<ENTRY
title NuOS Installer (live)
linux /vmlinuz
initrd /initrd.img
options root=/dev/ram0 console=tty1 console=ttyS0,115200
ENTRY
    mcopy -i "$ISO_ESP" "$WORK/nuos-live.conf" ::loader/entries/nuos-live.conf
    mkdir -p "$WORK/iso-root"
    echo "NuOS installer ISO - boot via UEFI" > "$WORK/iso-root/README.txt"
    xorriso -as mkisofs -V NUOS -r -J -joliet-long -o "$ISO_OUT"         --append_partition 2 0xef "$ISO_ESP" "$WORK/iso-root" 2>&1 | tail -2
    echo "==> done: $ISO_OUT ($(stat -c %s "$ISO_OUT") bytes)"
fi

echo "==> partitioning $OUT"
rm -f "$OUT"
truncate -s "$SIZE" "$OUT"   # sparse; fallocate unsupported on some filesystems
SIZE_BYTES=$(stat -c %s "$OUT")
TOTAL_SECTORS=$((SIZE_BYTES / 512))
ROOT_SECTORS=$((TOTAL_SECTORS - 526336 - 2048))  # 1MiB tail for GPT backup
sfdisk --quiet "$OUT" <<SFDISK
label: gpt
unit: sectors
first-lba: 2048
start=2048, size=524288, type=c12a7328-f81f-11d2-ba4b-00a0c93ec93b, uuid=$NUOS_ESP_PARTUUID, name="ESP"
start=526336, size=$ROOT_SECTORS, type=0fc63daf-8483-4772-8e79-3d69d8477de4, uuid=$NUOS_ROOT_PARTUUID, name="root"
SFDISK

# --- ESP (FAT, populated with mtools — no mount) ------------------------------
echo "==> building ESP"
truncate -s 256M "$ESP"
mkfs.vfat -F 32 -n ESP "$ESP"
mmd   -i "$ESP" ::EFI ::EFI/BOOT ::loader ::loader/entries
SB_EFI="$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
[ -f "$SB_EFI" ] || SB_EFI="$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootx64.efi.signed"
mcopy -i "$ESP" -s -Q "$SB_EFI" ::EFI/BOOT/BOOTX64.EFI
cp "$ROOTFS/boot/vmlinuz-$KVER" "$WORK/vmlinuz"
cp "$ROOTFS/boot/initrd.img-$KVER" "$WORK/initrd.img"
mcopy -i "$ESP" "$WORK/vmlinuz" ::vmlinuz
mcopy -i "$ESP" "$WORK/initrd.img" ::initrd.img
printf 'timeout 3\ndefault nuos\n' > "$WORK/loader.conf"
mcopy -i "$ESP" "$WORK/loader.conf" ::loader/loader.conf
cat > "$WORK/nuos.conf" <<ENTRY
title NuOS
linux /vmlinuz
initrd /initrd.img
options $FULL_CMDLINE
ENTRY
mcopy -i "$ESP" "$WORK/nuos.conf" ::loader/entries/nuos.conf
dd if="$ESP" of="$OUT" bs=512 seek=2048 conv=notrunc status=none

# --- rootfs (ext4 via mke2fs -d — no mount) -----------------------------------
echo "==> populating ext4 root partition"
ROOT_BLOCKS=$((ROOT_SECTORS * 512 / 4096))
ROOT_START_BYTES=$((526336 * 512))
mkfs.ext4 -q -F -b 4096 -L nuos-root \
    -E offset="$ROOT_START_BYTES" \
    -d "$ROOTFS" \
    "$OUT" "$ROOT_BLOCKS"
echo "NOTE: ext4 created without journal (flash-friendliness); add journal on device if desired." >&2

echo "==> done: $OUT ($(stat -c %s "$OUT") bytes)"
echo "    cmdline: $FULL_CMDLINE"
