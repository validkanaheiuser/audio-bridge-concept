/**
 * Audio Bridge - Shared Header
 * Version: 3.0
 * License: MIT
 * 
 * Contains shared type definitions used by the native daemon,
 * JNI bridge, and Zygisk module.
 */

#ifndef AUDIO_BRIDGE_H
#define AUDIO_BRIDGE_H

#include <stdint.h>
#include <atomic>

// ──────────────────────────────────────────────────────────────────────────
// Version
// ──────────────────────────────────────────────────────────────────────────

#define VERSION_MAJOR 3
#define VERSION_MINOR 0
#define VERSION_PATCH 0

// ──────────────────────────────────────────────────────────────────────────
// Audio Configuration Constants
// ──────────────────────────────────────────────────────────────────────────

static const int SAMPLE_RATE      = 48000;
static const int CHANNELS         = 1;
static const int FRAME_MS         = 20;
static const int FRAME_SAMPLES    = (SAMPLE_RATE * FRAME_MS / 1000); // 960
static const int FRAME_BYTES      = FRAME_SAMPLES * sizeof(int16_t);
static const int MAX_PKT          = 4000;
static const int JITTER_FRAMES    = 6;
static const int SHM_RING_SIZE    = 64;
static const int SHM_SIZE         = 1024 * 1024;

// ──────────────────────────────────────────────────────────────────────────
// Frame Types (Multiplex Protocol)
// ──────────────────────────────────────────────────────────────────────────

enum FrameType : uint8_t {
    T_SPEAKER     = 0x01,  // Phone speaker → Server
    T_VIRTUAL_MIC = 0x02,  // Server → Phone virtual mic
    T_CONTROL     = 0x03,  // Control messages
    T_CALL_STATUS = 0x04,  // Call status updates
    T_SMS         = 0x05,  // SMS control and status
    T_PING        = 0x06,  // Keepalive ping
    T_PONG        = 0x07,  // Keepalive pong
    T_ERROR       = 0xFF   // Error response
};

// ──────────────────────────────────────────────────────────────────────────
// Call States
// ──────────────────────────────────────────────────────────────────────────

enum CallState : int {
    CALL_IDLE     = 0,
    CALL_RINGING  = 1,
    CALL_OFFHOOK  = 2,
    CALL_DIALING  = 3,
    CALL_HOLDING  = 4
};

// ──────────────────────────────────────────────────────────────────────────
// Shared Memory Structures (Must match Zygisk module)
// ──────────────────────────────────────────────────────────────────────────

// AudioFrame.flags bits. The low byte tags the source of the samples so the
// server can route mixed audio (e.g., draw cellular call audio in a
// separate UI lane from app/VoIP audio). Bits 8–31 are reserved.
static const uint32_t FRAME_FLAG_ORIGIN_MASK = 0x00FFu;
static const uint32_t FRAME_FLAG_ORIGIN_APP  = 0x0001u; // app AudioTrack/Record
static const uint32_t FRAME_FLAG_ORIGIN_CELL = 0x0002u; // cellular call (VOICE_CALL src or HAL hook)

struct AudioFrame {
    int16_t data[FRAME_SAMPLES];
    uint64_t timestamp;
    uint32_t flags;
    uint32_t reserved;
};

struct SharedMemoryLayout {
    std::atomic<uint32_t> write_index;
    std::atomic<uint32_t> read_index;
    std::atomic<uint32_t> speaker_write_idx;
    std::atomic<uint32_t> speaker_read_idx;
    std::atomic<bool> module_active;
    std::atomic<bool> audio_capturing;
    std::atomic<uint64_t> last_activity;
    uint32_t padding[4];
    AudioFrame mic_frames[SHM_RING_SIZE];
    AudioFrame speaker_frames[SHM_RING_SIZE];
};

// ──────────────────────────────────────────────────────────────────────────
// Logging Macros
// ──────────────────────────────────────────────────────────────────────────

#ifdef ANDROID
#include <android/log.h>
#define LOG_TAG "AudioBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif

#endif // AUDIO_BRIDGE_H
