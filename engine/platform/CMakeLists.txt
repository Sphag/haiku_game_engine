# In-house windowing (US-007 Win32, US-008 X11) and input (US-009 Win32, US-010 X11/evdev). The common API, DPI math,
# slot table, event queue, input state, input mapping, evdev decoding, and actions (US-011: action maps, input router,
# input profiles) are platform-neutral and compile everywhere; exactly one backend is built per operating system.
if(WIN32)
  set(engine_platform_backend src/win32/platform_win32.cpp src/win32/input_win32.cpp src/win32/dialogs_win32.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # X11 backend. libX11.so.6 is loaded with dlopen (xlib_abi.cpp), so no X11 headers or -dev packages are needed.
  set(engine_platform_backend src/x11/platform_x11.cpp src/x11/xlib_abi.cpp src/x11/gamepads_linux.cpp src/dialogs_other.cpp)
else()
  set(engine_platform_backend src/platform_unsupported.cpp src/dialogs_other.cpp)
endif()

add_library(engine_platform STATIC
  src/action_map.cpp
  src/action_profile.cpp
  src/event_queue.cpp
  src/evdev.cpp
  src/input_mapping.cpp
  src/input_names.cpp
  src/input_router.cpp
  src/input_state.cpp
  src/window.cpp
  ${engine_platform_backend}
)
target_include_directories(engine_platform PUBLIC include)
target_link_libraries(engine_platform PUBLIC engine_core)
# Input profiles read and write JSON with the vendored nlohmann/json (PRD: Dependency Catalog). It is a system include,
# so its warnings stay out of the first-party -Werror policy, and only action_profile.cpp includes it.
target_include_directories(engine_platform SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
if(WIN32)
  # Win32 system import libraries, listed explicitly as permitted OS ABIs (PRD: Technical Considerations).
  # user32 for windows and messages; xinput for controller state (XInputGetState).
  target_link_libraries(engine_platform PRIVATE user32 xinput ole32 shell32 uuid)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # dlopen/dlsym bind libX11.so.6 and libXi.so.6 at run time. Listed explicitly as a system ABI (PRD: Dependency Catalog).
  target_link_libraries(engine_platform PRIVATE ${CMAKE_DL_LIBS})
endif()
engine_configure_first_party(engine_platform)
