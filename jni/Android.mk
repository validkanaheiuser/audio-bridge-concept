# Audio Bridge - JNI Android.mk

LOCAL_PATH := $(call my-dir)

# ──────────────────────────────────────────────────────────────────────────
# Prebuilt Opus
# ──────────────────────────────────────────────────────────────────────────

include $(CLEAR_VARS)
LOCAL_MODULE := opus
LOCAL_SRC_FILES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/libopus.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/include
include $(PREBUILT_STATIC_LIBRARY)

# ──────────────────────────────────────────────────────────────────────────
# Prebuilt TinyALSA
# ──────────────────────────────────────────────────────────────────────────

include $(CLEAR_VARS)
LOCAL_MODULE := tinyalsa
LOCAL_SRC_FILES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/libtinyalsa.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/include
include $(PREBUILT_STATIC_LIBRARY)

# ──────────────────────────────────────────────────────────────────────────
# Prebuilt mbedtls
# ──────────────────────────────────────────────────────────────────────────

include $(CLEAR_VARS)
LOCAL_MODULE := mbedtls
LOCAL_SRC_FILES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/libmbedtls.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/include
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := mbedcrypto
LOCAL_SRC_FILES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/libmbedcrypto.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := mbedx509
LOCAL_SRC_FILES := $(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/libmbedx509.a
include $(PREBUILT_STATIC_LIBRARY)

# ──────────────────────────────────────────────────────────────────────────
# Main Audio Bridge Binary
# ──────────────────────────────────────────────────────────────────────────

include $(CLEAR_VARS)
LOCAL_MODULE := audio-bridge
LOCAL_SRC_FILES := audio_bridge.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_STATIC_LIBRARIES := opus tinyalsa mbedtls mbedcrypto mbedx509
LOCAL_LDLIBS := -llog -lpthread
LOCAL_CPPFLAGS := -std=c++17 -O3 -fPIE -DANDROID
LOCAL_LDFLAGS := -pie -Wl,--gc-sections -Wl,--strip-all
include $(BUILD_EXECUTABLE)

# ──────────────────────────────────────────────────────────────────────────
# JNI Shared Library
# ──────────────────────────────────────────────────────────────────────────

include $(CLEAR_VARS)
LOCAL_MODULE := audiobridge
LOCAL_SRC_FILES := jni_bridge.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_LDLIBS := -llog
LOCAL_CPPFLAGS := -std=c++17 -O3 -fPIC -DANDROID
include $(BUILD_SHARED_LIBRARY)
