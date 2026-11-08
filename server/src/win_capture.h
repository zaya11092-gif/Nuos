// X11 window enumeration + per-window capture (NuOS v2).
// X11 (not Wayland) required: the board's KDE session runs under X11.
#ifndef NUOS_WIN_CAPTURE_H
#define NUOS_WIN_CAPTURE_H

#include <cstdint>
#include <string>
#include <vector>

struct WinInfo {
    uint32_t id;
    std::string title;
    int w, h;
};

struct WinImage {
    std::vector<uint8_t> data;  // BGRA
    uint32_t stride;
    uint32_t width, height;
};

// Opens the display (DISPLAY env or :0). Returns false if X11 unavailable.
bool nuos_x11_init();

// List candidate windows (mapped, with titles, not docks/desktops).
std::vector<WinInfo> nuos_list_windows();

// Capture one window's client area as BGRA (composited — works even when
// partially occluded, as long as the compositor is running).
bool nuos_capture_window(uint32_t window_id, WinImage& out);

// JSON array of windows for the phone: [{"id":123,"title":"...","w":..,"h":..}]
std::string nuos_list_windows_json();

#endif
