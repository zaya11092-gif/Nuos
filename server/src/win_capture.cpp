#include "win_capture.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <mutex>
#include <sstream>

namespace {
Display* g_dpy = nullptr;
std::mutex g_xmtx;  // Xlib is not thread-safe across our threads

void enum_windows_cb(Display* dpy, Window w, int depth,
                     std::vector<WinInfo>& out) {
    Window root, parent;
    Window* children = nullptr;
    unsigned int nchild = 0;
    XQueryTree(dpy, w, &root, &parent, &children, &nchild);
    if (children) XFree(children);

    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return;
    if (attr.map_state != IsViewable) return;
    if (attr.width < 64 || attr.height < 64) return;

    // Skip windows without a WM_NAME (docks/desktops tend to be nameless)
    char* name = nullptr;
    XFetchName(dpy, w, &name);
    if (!name || !*name) {
        if (name) XFree(name);
    } else {
        WinInfo info;
        info.id = (uint32_t)w;
        info.title = name;
        info.w = attr.width;
        info.h = attr.height;
        XFree(name);
        out.push_back(info);
    }
    for (int d = 0; d < depth; ++d) {}
}

void walk(Display* dpy, Window w, int depth, std::vector<WinInfo>& out) {
    enum_windows_cb(dpy, w, depth, out);
    Window root, parent;
    Window* children = nullptr;
    unsigned int nchild = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &children, &nchild)) return;
    for (unsigned int i = 0; i < nchild; ++i)
        walk(dpy, children[i], depth + 1, out);
    if (children) XFree(children);
}
}  // namespace

bool nuos_x11_init() {
    std::lock_guard<std::mutex> lk(g_xmtx);
    const char* disp = getenv("DISPLAY");
    g_dpy = XOpenDisplay(disp && *disp ? disp : ":0");
    return g_dpy != nullptr;
}

std::vector<WinInfo> nuos_list_windows() {
    std::vector<WinInfo> out;
    if (!g_dpy) return out;
    std::lock_guard<std::mutex> lk(g_xmtx);
    walk(g_dpy, DefaultRootWindow(g_dpy), 0, out);
    return out;
}

std::string nuos_list_windows_json() {
    std::ostringstream os;
    os << "[";
    auto wins = nuos_list_windows();
    bool first = true;
    for (auto& w : wins) {
        if (!first) os << ",";
        first = false;
        // escape quotes/backslashes in title
        std::string t;
        for (char c : w.title) {
            if (c == '"' || c == '\\') t += '\\';
            t += c;
        }
        os << "{\"id\":" << w.id << ",\"title\":\"" << t << "\",\"w\":" << w.w
           << ",\"h\":" << w.h << "}";
    }
    os << "]";
    return os.str();
}

bool nuos_capture_window(uint32_t window_id, WinImage& out) {
    if (!g_dpy) return false;
    std::lock_guard<std::mutex> lk(g_xmtx);
    Window w = (Window)window_id;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(g_dpy, w, &attr)) return false;
    XImage* img = XGetImage(g_dpy, w, 0, 0, attr.width, attr.height, AllPlanes,
                            ZPixmap);
    if (!img) return false;
    out.width = (uint32_t)attr.width;
    out.height = (uint32_t)attr.height;
    out.stride = (uint32_t)img->bytes_per_line;
    size_t sz = (size_t)out.stride * out.height;
    out.data.assign(img->data, img->data + sz);
    XDestroyImage(img);
    return true;
}
