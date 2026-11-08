#!/bin/bash
# Build + install the NuOS OpenTrack driver fork (HMD + 2 pinch controllers)
# into SteamVR's drivers directory on the board.
set -x
cd ~/nuos2
if [ ! -d opentrack-driver ]; then
  git clone --recurse-submodules https://github.com/pbzweihander/OpenVR-OpenTrack opentrack-driver || exit 1
fi
cp ~/nuos2/server/driver-fork/hmd_device_driver.h  opentrack-driver/src/
cp ~/nuos2/server/driver-fork/hmd_device_driver.cpp opentrack-driver/src/
cp ~/nuos2/server/driver-fork/device_provider.h    opentrack-driver/src/
cp ~/nuos2/server/driver-fork/device_provider.cpp  opentrack-driver/src/
cd opentrack-driver
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build 2>&1 | tail -3 || { echo DRIVER_BUILD_FAIL; exit 1; }
SO=$(find build -name "*.so" | head -1)
[ -n "$SO" ] || { echo NO_SO; exit 1; }

# SteamVR drivers dir (both common Steam layouts)
for SD in ~/.local/share/Steam/steamapps/common/SteamVR /home/a/.steam/steam/steamapps/common/SteamVR; do
  if [ -d "$SD" ]; then
    echo 1 | sudo -S mkdir -p "$SD/drivers/nuoshmd/bin/linux64" "$SD/drivers/nuoshmd/resources/settings"
    echo 1 | sudo -S cp "$SO" "$SD/drivers/nuoshmd/bin/linux64/driver_nuoshmd.so"
    echo 1 | sudo -S tee "$SD/drivers/nuoshmd/driver.vrdrivermanifest" > /dev/null <<M
"driver"
{
  "name" "nuoshmd"
  "internal_name" "driver_opentrack"
  "directory" "nuoshmd"
  "resource_only_driver" "0"
  "hmd_presence" "nuos_hmd"
}
M
    echo 1 | sudo -S tee "$SD/drivers/nuoshmd/resources/settings/default.vrsettings" > /dev/null <<S
{
  "driver_opentrack" : {
    "serial_number" : "nuos_hmd_001",
    "model_number" : "NuOS Phone HMD",
    "window_x" : 0,
    "window_y" : 0,
    "window_width" : 2560,
    "window_height" : 720,
    "render_width" : 2048,
    "render_height" : 1024,
    "force_bounds" : 0
  }
}
S
    echo "installed to $SD/drivers/nuoshmd"
    INSTALLED=1
  fi
done
[ -n "$INSTALLED" ] || echo "SteamVR dir not found — install SteamVR first, then re-run"
echo DRIVER_DEPLOY_END
