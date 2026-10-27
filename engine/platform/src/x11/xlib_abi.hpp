#pragma once

// The subset of the Xlib ABI used by the in-house X11 backend (US-008), for LP64 Linux. The Xlib headers are not
// needed to build: libX11.so.6 is bound at run time with dlopen, so a machine without X11 libraries or without a
// display still builds and runs its CPU code. Layouts mirror <X11/Xlib.h> and must stay in sync with it.
#include <cstdint>

namespace engine::platform::x11 {

struct _XDisplay;
using Display = _XDisplay;
using XID = unsigned long;
using Window = XID;
using Atom = unsigned long;
using Bool = int;
using Status = int;
using KeyCode = unsigned int;
using KeySym = unsigned long;
using Time = unsigned long;
using Cursor = XID;
using Pixmap = XID;
using XIC = struct _XIC*;
using XIM = struct _XIM*;

inline constexpr int kNone = 0;
inline constexpr int kCopyFromParent = 0;
inline constexpr int kInputOutput = 1;
inline constexpr int kPropModeReplace = 0;
inline constexpr int kRevertToParent = 2;
inline constexpr unsigned long kCurrentTime = 0;
inline constexpr int kNotifyNormal = 0;
inline constexpr int kNotifyInferior = 2;
inline constexpr int kNotifyPointer = 5;
inline constexpr int kNotifyPointerRoot = 6;
inline constexpr int kNotifyDetailNone = 7;
inline constexpr int kSuccess = 0;
inline constexpr int kGrabSuccess = 0;
inline constexpr int kGrabModeAsync = 1;
inline constexpr int kXBufferOverflow = -1;
inline constexpr long kPMinSize = 1L << 4;
inline constexpr long kPMaxSize = 1L << 5;

// Event masks for XSelectInput and XGrabPointer.
inline constexpr long kKeyPressMask = 1L << 0;
inline constexpr long kKeyReleaseMask = 1L << 1;
inline constexpr long kButtonPressMask = 1L << 2;
inline constexpr long kButtonReleaseMask = 1L << 3;
inline constexpr long kPointerMotionMask = 1L << 6;
inline constexpr long kStructureNotifyMask = 1L << 17;
inline constexpr long kFocusChangeMask = 1L << 21;

// Input method styles for XCreateIC. Preedit and status are drawn by the application, which draws nothing here.
inline constexpr unsigned long kIMPreeditNothing = 0x0008L;
inline constexpr unsigned long kIMStatusNothing = 0x0400L;
inline constexpr const char* kNInputStyle = "inputStyle";
inline constexpr const char* kNClientWindow = "clientWindow";
inline constexpr const char* kNFocusWindow = "focusWindow";

// XI2 event type for a raw (unaccelerated) pointer motion, and the device id that selects every device.
inline constexpr int kXIRawMotion = 17;
inline constexpr int kXIAllDevices = 0;

// Event types.
inline constexpr int kKeyPress = 2;
inline constexpr int kKeyRelease = 3;
inline constexpr int kButtonPress = 4;
inline constexpr int kButtonRelease = 5;
inline constexpr int kMotionNotify = 6;
inline constexpr int kFocusIn = 9;
inline constexpr int kFocusOut = 10;
inline constexpr int kUnmapNotify = 18;
inline constexpr int kMapNotify = 19;
inline constexpr int kConfigureNotify = 22;
inline constexpr int kClientMessage = 33;
inline constexpr int kGenericEvent = 35;

struct XAnyEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
};

struct XConfigureEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window event;
    Window window;
    int x;
    int y;
    int width;
    int height;
    int border_width;
    Window above;
    Bool override_redirect;
};

struct XFocusChangeEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    int mode;
    int detail;
};

// KeyPress/KeyRelease and ButtonPress/ButtonRelease share this layout up to the button or keycode field.
struct XKeyEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int keycode;
    Bool same_screen;
};

struct XButtonEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int button;
    Bool same_screen;
};

struct XMotionEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    char is_hint;
    Bool same_screen;
};

// Generic events (XI2 among them) carry their payload behind a cookie that XGetEventData fills in.
struct XGenericEventCookie {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    int extension;
    int evtype;
    unsigned int cookie;
    void* data;
};

struct XClientMessageEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    Atom message_type;
    int format;
    union {
        char b[20];
        short s[10];
        long l[5];
    } data;
};

// Large enough for every event Xlib can return (the real XEvent is 24 longs).
union XEvent {
    int type;
    long pad[24];
    XAnyEvent xany;
    XKeyEvent xkey;
    XButtonEvent xbutton;
    XMotionEvent xmotion;
    XConfigureEvent xconfigure;
    XFocusChangeEvent xfocus;
    XClientMessageEvent xclient;
    XGenericEventCookie xcookie;
};

// Colour for XCreatePixmapCursor. The layout matches Xlib's XColor.
struct XColor {
    unsigned long pixel;
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    char flags;
    char pad;
};

struct XErrorEvent {
    int type;
    Display* display;
    XID resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};

struct XSizeHints {
    long flags;
    int x;
    int y;
    int width;
    int height;
    int min_width;
    int min_height;
    int max_width;
    int max_height;
    int width_inc;
    int height_inc;
    struct {
        int x;
        int y;
    } min_aspect;
    struct {
        int x;
        int y;
    } max_aspect;
    int base_width;
    int base_height;
    int win_gravity;
};

using XErrorHandler = int (*)(Display*, XErrorEvent*);

// XI2 structs, as in <X11/extensions/XInput2.h>. The valuator mask marks which valuators a raw event carries.
struct XIEventMask {
    int deviceid;
    int mask_len;
    unsigned char* mask;
};

struct XIValuatorState {
    int mask_len;
    unsigned char* mask;
    double* values;
};

// Payload of a raw motion event, reached through XGenericEventCookie::data.
struct XIRawEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    int extension;
    int evtype;
    Time time;
    int detail;
    int deviceid;
    int sourceid;
    int flags;
    XIValuatorState valuators;
    double* raw_values;
};


// Function pointers bound from libX11.so.6. Names match the Xlib functions they call.
struct Api {
    Display* (*XOpenDisplay)(const char* name);
    int (*XCloseDisplay)(Display* display);
    int (*XDefaultScreen)(Display* display);
    Window (*XDefaultRootWindow)(Display* display);
    Window (*XCreateWindow)(Display* display, Window parent, int x, int y, unsigned int width, unsigned int height,
                            unsigned int border_width, int depth, unsigned int window_class, void* visual,
                            unsigned long valuemask, void* attributes);
    int (*XDestroyWindow)(Display* display, Window window);
    int (*XMapRaised)(Display* display, Window window);
    Status (*XIconifyWindow)(Display* display, Window window, int screen);
    int (*XChangeProperty)(Display* display, Window window, Atom property, Atom type, int format, int mode,
                           const unsigned char* data, int elements);
    Atom (*XInternAtom)(Display* display, const char* name, Bool only_if_exists);
    int (*XSelectInput)(Display* display, Window window, long event_mask);
    Status (*XSetWMProtocols)(Display* display, Window window, Atom* protocols, int count);
    void (*XSetWMNormalHints)(Display* display, Window window, XSizeHints* hints);
    int (*XPending)(Display* display);
    int (*XNextEvent)(Display* display, XEvent* event);
    int (*XFlush)(Display* display);
    int (*XSync)(Display* display, Bool discard);
    Status (*XGetGeometry)(Display* display, Window window, Window* root, int* x, int* y, unsigned int* width,
                           unsigned int* height, unsigned int* border_width, unsigned int* depth);
    char* (*XGetDefault)(Display* display, const char* program, const char* option);
    XErrorHandler (*XSetErrorHandler)(XErrorHandler handler);
    int (*XGetErrorText)(Display* display, int code, char* buffer, int length);

    // Keyboard and text. Keycode lookups use group 0, the unshifted keysym of a key's position.
    KeySym (*XKeycodeToKeysym)(Display* display, KeyCode keycode, int index);
    KeyCode (*XKeysymToKeycode)(Display* display, KeySym keysym);
    Bool (*XkbSetDetectableAutoRepeat)(Display* display, Bool detectable, Bool* supported);
    Bool (*XQueryExtension)(Display* display, const char* name, int* major_opcode, int* first_event,
                            int* first_error);
    Bool (*XGetEventData)(Display* display, XGenericEventCookie* cookie);
    void (*XFreeEventData)(Display* display, XGenericEventCookie* cookie);

    // Input methods (XIM). XCreateIC is variadic: name/value pairs ending in a null pointer.
    char* (*XSetLocaleModifiers)(const char* modifiers);
    XIM (*XOpenIM)(Display* display, void* database, char* res_name, char* res_class);
    Status (*XCloseIM)(XIM im);
    XIC (*XCreateIC)(XIM im, ...);
    void (*XDestroyIC)(XIC ic);
    void (*XSetICFocus)(XIC ic);
    void (*XUnsetICFocus)(XIC ic);
    Bool (*XFilterEvent)(XEvent* event, Window window);
    int (*Xutf8LookupString)(XIC ic, XKeyEvent* event, char* buffer, int bytes, KeySym* keysym, Status* status);

    // Pointer capture and the invisible cursor used while captured.
    int (*XGrabPointer)(Display* display, Window window, Bool owner_events, unsigned int event_mask, int pointer_mode,
                        int keyboard_mode, Window confine_to, Cursor cursor, Time time);
    int (*XUngrabPointer)(Display* display, Time time);
    int (*XWarpPointer)(Display* display, Window source, Window destination, int source_x, int source_y,
                        unsigned int source_width, unsigned int source_height, int destination_x, int destination_y);
    // Reports the pointer's root and window-relative position; the window return is the window's own coordinates.
    int (*XQueryPointer)(Display* display, Window window, Window* root_return, Window* child_return, int* root_x, int* root_y,
                         int* window_x, int* window_y, unsigned int* mask_return);
    int (*XDefineCursor)(Display* display, Window window, Cursor cursor);
    int (*XUndefineCursor)(Display* display, Window window);
    Pixmap (*XCreateBitmapFromData)(Display* display, Window drawable, const char* data, unsigned int width,
                                    unsigned int height);
    Cursor (*XCreatePixmapCursor)(Display* display, Pixmap source, Pixmap mask, XColor* foreground, XColor* background,
                                  unsigned int x, unsigned int y);
    int (*XFreeCursor)(Display* display, Cursor cursor);
    int (*XFreePixmap)(Display* display, Pixmap pixmap);
};

// XI2 (libXi.so.6). Optional: without it the backend has no raw relative motion and reports that in the log.
struct XiApi {
    Status (*XIQueryVersion)(Display* display, int* major, int* minor);
    Status (*XISelectEvents)(Display* display, Window window, XIEventMask* masks, int count);
};

// Binds the functions on first use. Returns nullptr and sets `error` when libX11.so.6 or a symbol is missing.
// The library stays loaded for the life of the process.
[[nodiscard]] const Api* load_api(const char** error);

// Binds libXi.so.6 for XI2. Returns nullptr and sets `error` when the library or a symbol is missing.
[[nodiscard]] const XiApi* load_xi_api(const char** error);

}  // namespace engine::platform::x11
