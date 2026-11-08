#!/bin/bash
# NuOS v2 board setup — idempotent, for the existing Ubuntu install.
set -x

echo 1 | sudo -S apt-get update -qq

# Steam (multiverse) + i386 runtime
echo 1 | sudo -S add-apt-repository -y multiverse >/dev/null 2>&1 || true
echo 1 | sudo -S dpkg --add-architecture i386 2>/dev/null || true
echo 1 | sudo -S apt-get update -qq
echo 1 | sudo -S apt-get install -y -qq steam >/dev/null 2>&1 || \
  echo "steam install skipped (verify manually)"

# X11 session default (KDE on X11; per-window capture needs X11)
mkdir -p /var/lib/AccountsService/users 2>/dev/null || true
echo 1 | sudo -S bash -c 'mkdir -p /etc/sddm.conf.d && printf "[General]\nDisplayServer=x11\n[Wayland]\nEnabled=false\n" > /etc/sddm.conf.d/nuos-x11.conf'

# build + capture deps
echo 1 | sudo -S apt-get install -y -qq \
  build-essential cmake ninja-build pkg-config \
  libavcodec-dev libavutil-dev libswscale-dev libx11-dev \
  libgl1-mesa-dri mesa-vulkan-drivers intel-media-va-driver-non-free \
  x11-xserver-utils wmctrl xdotool >/dev/null 2>&1

# VAAPI sanity
echo 1 | sudo -S vainfo --display drm --device /dev/dri/renderD128 2>&1 | \
  grep -aE "HEVC.*Enc|driver version" | head -4 || true

echo SETUP_PACKAGES_DONE
