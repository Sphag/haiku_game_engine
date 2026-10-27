# The sound runtime (US-057). Decoding (WAV, Ogg Vorbis via stb_vorbis), the offline mixer, and the device (miniaudio) live here.
# The vendored C sources build in their own target, outside the first-party warning policy, the way the Lua VM does. The first-party
# shims around them stay C++-free.
set(AUDIO_C_SOURCES
  src/native/miniaudio_device.c
  src/native/vorbis_stream.c
)
add_library(engine_audio_native STATIC ${AUDIO_C_SOURCES})
# miniaudio.h is included by name, and stb_vorbis.c includes "stb/...", so the roots are the vendor directories themselves.
target_include_directories(engine_audio_native SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniaudio ${PROJECT_SOURCE_DIR}/third_party)
target_include_directories(engine_audio_native PRIVATE src/native)
set_target_properties(engine_audio_native PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(engine_audio_native PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
if(WIN32)
  target_link_libraries(engine_audio_native PRIVATE ole32)
endif()

add_library(engine_audio STATIC
  src/audio_clip.cpp
  src/music_stream.cpp
  src/mixer.cpp
  src/audio_engine.cpp
  src/cell_audio.cpp
)
target_include_directories(engine_audio PUBLIC include)
target_include_directories(engine_audio PRIVATE src)
target_link_libraries(engine_audio PUBLIC engine_core engine_world)
target_link_libraries(engine_audio PRIVATE engine_audio_native)
engine_configure_first_party(engine_audio)
