cmake_minimum_required(VERSION 3.18)
project(AudioBridge VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android NDK settings
if(ANDROID)
    set(CMAKE_ANDROID_API 28)
    set(CMAKE_ANDROID_STL c++_shared)
endif()

# ──────────────────────────────────────────────────────────────────────────
# Find dependencies
# ──────────────────────────────────────────────────────────────────────────

# Opus
find_library(OPUS_LIB opus PATHS ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI})
find_path(OPUS_INCLUDE opus/opus.h PATHS ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/include)

# TinyALSA
find_library(TINYALSA_LIB tinyalsa PATHS ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI})
find_path(TINYALSA_INCLUDE tinyalsa/asoundlib.h PATHS ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/include)

# ──────────────────────────────────────────────────────────────────────────
# Main native binary
# ──────────────────────────────────────────────────────────────────────────

add_executable(audio-bridge
    jni/audio_bridge.cpp
)

target_include_directories(audio-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/jni
    ${OPUS_INCLUDE}
    ${TINYALSA_INCLUDE}
)

target_link_libraries(audio-bridge
    ${OPUS_LIB}
    ${TINYALSA_LIB}
    log
    pthread
)

target_compile_options(audio-bridge PRIVATE -O3 -fPIE)
set_target_properties(audio-bridge PROPERTIES
    LINK_FLAGS "-pie -Wl,--gc-sections -Wl,--strip-all"
)

# ──────────────────────────────────────────────────────────────────────────
# JNI shared library
# ──────────────────────────────────────────────────────────────────────────

add_library(audiobridge SHARED
    jni/jni_bridge.cpp
)

target_include_directories(audiobridge PRIVATE
    ${CMAKE_SOURCE_DIR}/jni
)

target_link_libraries(audiobridge
    log
)

target_compile_options(audiobridge PRIVATE -O3 -fPIC)

# ──────────────────────────────────────────────────────────────────────────
# Zygisk module
# ──────────────────────────────────────────────────────────────────────────

add_library(zygisk_module SHARED
    zygisk/src/zygisk_module.cpp
)

target_include_directories(zygisk_module PRIVATE
    ${CMAKE_SOURCE_DIR}/zygisk
    ${CMAKE_SOURCE_DIR}/jni
)

target_link_libraries(zygisk_module
    log
)

target_compile_options(zygisk_module PRIVATE -O3 -fPIC)
