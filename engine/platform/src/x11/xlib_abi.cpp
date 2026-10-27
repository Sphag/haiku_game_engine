#include "xlib_abi.hpp"

#include <dlfcn.h>

#include <cstring>
#include <string>

namespace engine::platform::x11 {

namespace {

// Copies a symbol address into a function pointer without a cast that the pedantic warnings reject.
template <typename Fn>
bool bind(void* library, const char* name, Fn& out, const char** error) {
    void* symbol = dlsym(library, name);
    if (symbol == nullptr) {
        *error = name;
        return false;
    }
    static_assert(sizeof(Fn) == sizeof(void*), "function pointers must be pointer-sized");
    std::memcpy(&out, &symbol, sizeof(out));
    return true;
}

bool bind_all(void* library, Api& api, const char** error) {
    return bind(library, "XOpenDisplay", api.XOpenDisplay, error) &&
           bind(library, "XCloseDisplay", api.XCloseDisplay, error) &&
           bind(library, "XDefaultScreen", api.XDefaultScreen, error) &&
           bind(library, "XDefaultRootWindow", api.XDefaultRootWindow, error) &&
           bind(library, "XCreateWindow", api.XCreateWindow, error) &&
           bind(library, "XDestroyWindow", api.XDestroyWindow, error) &&
           bind(library, "XMapRaised", api.XMapRaised, error) &&
           bind(library, "XIconifyWindow", api.XIconifyWindow, error) &&
           bind(library, "XChangeProperty", api.XChangeProperty, error) &&
           bind(library, "XInternAtom", api.XInternAtom, error) &&
           bind(library, "XSelectInput", api.XSelectInput, error) &&
           bind(library, "XSetWMProtocols", api.XSetWMProtocols, error) &&
           bind(library, "XSetWMNormalHints", api.XSetWMNormalHints, error) &&
           bind(library, "XPending", api.XPending, error) &&
           bind(library, "XNextEvent", api.XNextEvent, error) &&
           bind(library, "XFlush", api.XFlush, error) &&
           bind(library, "XSync", api.XSync, error) &&
           bind(library, "XGetGeometry", api.XGetGeometry, error) &&
           bind(library, "XGetDefault", api.XGetDefault, error) &&
           bind(library, "XSetErrorHandler", api.XSetErrorHandler, error) &&
           bind(library, "XGetErrorText", api.XGetErrorText, error) &&
           bind(library, "XKeycodeToKeysym", api.XKeycodeToKeysym, error) &&
           bind(library, "XKeysymToKeycode", api.XKeysymToKeycode, error) &&
           bind(library, "XkbSetDetectableAutoRepeat", api.XkbSetDetectableAutoRepeat, error) &&
           bind(library, "XQueryExtension", api.XQueryExtension, error) &&
           bind(library, "XGetEventData", api.XGetEventData, error) &&
           bind(library, "XFreeEventData", api.XFreeEventData, error) &&
           bind(library, "XSetLocaleModifiers", api.XSetLocaleModifiers, error) &&
           bind(library, "XOpenIM", api.XOpenIM, error) &&
           bind(library, "XCloseIM", api.XCloseIM, error) &&
           bind(library, "XCreateIC", api.XCreateIC, error) &&
           bind(library, "XDestroyIC", api.XDestroyIC, error) &&
           bind(library, "XSetICFocus", api.XSetICFocus, error) &&
           bind(library, "XUnsetICFocus", api.XUnsetICFocus, error) &&
           bind(library, "XFilterEvent", api.XFilterEvent, error) &&
           bind(library, "Xutf8LookupString", api.Xutf8LookupString, error) &&
           bind(library, "XGrabPointer", api.XGrabPointer, error) &&
           bind(library, "XUngrabPointer", api.XUngrabPointer, error) &&
           bind(library, "XWarpPointer", api.XWarpPointer, error) &&
           bind(library, "XQueryPointer", api.XQueryPointer, error) &&
           bind(library, "XDefineCursor", api.XDefineCursor, error) &&
           bind(library, "XUndefineCursor", api.XUndefineCursor, error) &&
           bind(library, "XCreateBitmapFromData", api.XCreateBitmapFromData, error) &&
           bind(library, "XCreatePixmapCursor", api.XCreatePixmapCursor, error) &&
           bind(library, "XFreeCursor", api.XFreeCursor, error) &&
           bind(library, "XFreePixmap", api.XFreePixmap, error);
}

}  // namespace

const XiApi* load_xi_api(const char** error) {
    static const char* load_error = nullptr;
    static const XiApi* loaded = nullptr;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        // XI2 is optional, so a missing library is reported through `error` and never ends the Platform.
        void* library = dlopen("libXi.so.6", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            static const char* library_error = "libXi.so.6 could not be loaded";
            load_error = library_error;
        } else {
            static XiApi api{};
            const char* missing = nullptr;
            if (bind(library, "XIQueryVersion", api.XIQueryVersion, &missing) &&
                bind(library, "XISelectEvents", api.XISelectEvents, &missing)) {
                loaded = &api;
            } else {
                static std::string symbol_error;
                symbol_error = std::string("libXi.so.6 does not export ") + missing;
                load_error = symbol_error.c_str();
            }
        }
    }
    if (loaded == nullptr && error != nullptr) {
        *error = load_error;
    }
    return loaded;
}

const Api* load_api(const char** error) {
    static const char* load_error = nullptr;
    static const Api* loaded = nullptr;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        // Loaded once and never closed: Xlib keeps process-wide state that must outlive any Platform.
        void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            static const char* library_error = "libX11.so.6 could not be loaded";
            load_error = library_error;
        } else {
            static Api api{};
            const char* missing = nullptr;
            if (bind_all(library, api, &missing)) {
                loaded = &api;
            } else {
                static std::string symbol_error;
                symbol_error = std::string("libX11.so.6 does not export ") + missing;
                load_error = symbol_error.c_str();
            }
        }
    }
    if (loaded == nullptr && error != nullptr) {
        *error = load_error;
    }
    return loaded;
}

}  // namespace engine::platform::x11
