#include <jni.h>
#include <string>
#include <atomic>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <android/log.h>
#include <chrono>
#include <string.h>
#include <unistd.h>
#include <thread>
#include "zygisk.hpp"

// shadowhook (bytedance/android-inline-hook) — dlopen'd at runtime from the
// module's own install dir. We only need a couple of its symbols, so we
// declare the types locally and resolve via dlsym.
typedef enum { SHADOWHOOK_MODE_SHARED = 0, SHADOWHOOK_MODE_UNIQUE = 1 } shadowhook_mode_t;
using shadowhook_init_t            = int  (*)(shadowhook_mode_t, bool);
using shadowhook_hook_sym_name_t   = void*(*)(const char*, const char*, void*, void**);
using shadowhook_get_errno_t       = int  (*)(void);
using shadowhook_to_errmsg_t       = const char* (*)(int);
using shadowhook_dlopen_t          = void*(*)(const char*);
using shadowhook_dlsym_symtab_t    = void*(*)(void*, const char*);

static shadowhook_init_t           sh_init            = nullptr;
static shadowhook_hook_sym_name_t  sh_hook_sym_name   = nullptr;
static shadowhook_get_errno_t      sh_get_errno       = nullptr;
static shadowhook_to_errmsg_t      sh_to_errmsg       = nullptr;
static shadowhook_dlopen_t         sh_dlopen          = nullptr;
static shadowhook_dlsym_symtab_t   sh_dlsym_symtab    = nullptr;

// Module id as declared in module.prop. Used to locate libshadowhook.so on
// disk; see load_shadowhook() below.
static constexpr const char* kShadowhookPath =
    "/data/adb/modules/audio_bridge/zygisk/libshadowhook.so";

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AudioBridge-Zygisk", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "AudioBridge-Zygisk", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AudioBridge-Zygisk", __VA_ARGS__)

// Must match jni/audio_bridge.cpp
static const int SAMPLE_RATE   = 48000;
static const int FRAME_SAMPLES = 960;       // 20ms @ 48kHz
static const int FRAME_BYTES   = FRAME_SAMPLES * 2;
static const int RING_SIZE     = 64;

struct AudioFrame {
    int16_t  data[FRAME_SAMPLES];
    uint64_t timestamp;
    uint32_t flags;
    uint32_t reserved;
};

struct SharedMemoryLayout {
    std::atomic<uint32_t> write_index;
    std::atomic<uint32_t> read_index;
    std::atomic<uint32_t> speaker_write_idx;
    std::atomic<uint32_t> speaker_read_idx;
    std::atomic<bool>     module_active;
    std::atomic<bool>     audio_capturing;
    std::atomic<uint64_t> last_activity;
    uint32_t              padding[4];
    AudioFrame            mic_frames[RING_SIZE];
    AudioFrame            speaker_frames[RING_SIZE];
};

static SharedMemoryLayout* g_shm = nullptr;
static std::atomic<bool>   g_active{false};

// Per-process state for the read/write injection. AudioRecord may ask for
// buffer sizes that aren't exact multiples of FRAME_SAMPLES; we keep a leftover
// buffer so we don't waste samples.
struct LeftoverBuf {
    int16_t samples[FRAME_SAMPLES];
    size_t  offset = 0;          // next unread sample
    size_t  valid  = 0;          // total valid samples in buffer
};
static thread_local LeftoverBuf g_mic_leftover;

// Signatures used by Android 12+. shadowhook hooks by (lib, mangled symbol)
// and returns a stub; we just need the right function-pointer casts.
using AudioRecord_read_t          = ssize_t (*)(void*, void*, size_t, bool);
using AudioTrack_write_t          = ssize_t (*)(void*, const void*, size_t, bool);
using AudioRecord_getSampleRate_t = uint32_t (*)(const void*);
using AudioTrack_getSampleRate_t  = uint32_t (*)(const void*);

static AudioRecord_read_t          original_audio_record_read    = nullptr;
static AudioTrack_write_t          original_audio_track_write    = nullptr;
static AudioRecord_getSampleRate_t g_ar_get_sample_rate          = nullptr;
static AudioTrack_getSampleRate_t  g_at_get_sample_rate          = nullptr;

// ─── Linear sample-rate converter ──────────────────────────────────────────
// Pure streaming linear interpolator. Keeps one sample of history so chunk
// boundaries don't click. Quality is adequate for speech (8/16 kHz VoIP);
// upgrade to speex_resampler or soxr if music-quality is ever needed.
struct LinearSRC {
    uint32_t in_rate  = 0;
    uint32_t out_rate = 0;
    int16_t  last_in  = 0;
    // Position (in input samples) of the NEXT output sample to emit. When a
    // chunk of N samples arrives, the conceptual input timeline is
    // [last_in, in[0], in[1], ..., in[N-1]]; index 0 here means "last_in",
    // index 1 means in[0], etc. Phase is in [0, 1) at rest.
    double   phase    = 0.0;

    void configure(uint32_t i, uint32_t o) {
        if (i == in_rate && o == out_rate) return;
        in_rate = i; out_rate = o; last_in = 0; phase = 0.0;
    }

    size_t process(const int16_t* in, size_t n_in,
                   int16_t* out, size_t out_max) {
        if (n_in == 0) return 0;
        if (in_rate == out_rate) {
            size_t n = std::min(n_in, out_max);
            memcpy(out, in, n * 2);
            last_in = in[n - 1];
            return n;
        }
        const double step = (double)in_rate / (double)out_rate;
        // Padded input timeline: index 0 = last_in, index i ≥ 1 = in[i-1].
        size_t produced = 0;
        double p = phase;
        while (produced < out_max) {
            if (p >= (double)n_in) break;        // exhausted input
            size_t i0 = (size_t)p;
            double frac = p - (double)i0;
            int16_t a = (i0 == 0) ? last_in : in[i0 - 1];
            int16_t b = in[i0];
            int32_t s = (int32_t)a + (int32_t)((b - a) * frac);
            out[produced++] = (int16_t)s;
            p += step;
        }
        // Count the inputs we actually used (indices 0 .. floor(p)-1). Any
        // tail beyond that is dropped: for downsampling by ≤6× this is at
        // most ~step samples = ~120µs at 8kHz mic — imperceptible.
        size_t consumed = (size_t)p;
        if (consumed > n_in) consumed = n_in;
        if (consumed > 0) last_in = in[consumed - 1];
        phase = p - (double)consumed;
        if (phase < 0.0) phase = 0.0;
        return produced;
    }
};

static thread_local LinearSRC g_mic_src;      // 48k → app-rate
static thread_local LinearSRC g_spk_src;      // app-rate → 48k
// Accumulator for non-48k speaker chunks until we've got full 20ms 48kHz frames.
// Sized generously: upsampling 8kHz→48kHz is a 6× blow-up. A 512-sample input
// chunk yields up to ~3072 output samples; plus up to FRAME_SAMPLES carry-over.
static const  int  SPK_ACCUM_CAP = 4096;
static thread_local int16_t   g_spk_accum[SPK_ACCUM_CAP];
static thread_local size_t    g_spk_accum_n = 0;

// ─── Mic injection ─────────────────────────────────────────────────────────
// Strategy:
// 1) Always invoke original_read first so the HAL pipeline keeps draining.
// 2) If we have injection data, OVERWRITE the buffer with our samples.
// 3) Pull from the SHM ring one frame at a time, refilling a leftover buffer
//    for odd-sized requests.
// 4) If the ring underruns, zero-fill the remainder so the app doesn't stall.

static size_t pull_mic_samples(int16_t* out, size_t samples_needed) {
    size_t written = 0;

    // 1) Drain leftover first
    if (g_mic_leftover.offset < g_mic_leftover.valid) {
        size_t avail = g_mic_leftover.valid - g_mic_leftover.offset;
        size_t take  = std::min(avail, samples_needed);
        memcpy(out, g_mic_leftover.samples + g_mic_leftover.offset, take * 2);
        g_mic_leftover.offset += take;
        written += take;
    }

    // 2) Pull whole frames from the ring
    while (written < samples_needed) {
        uint32_t write_idx = g_shm->write_index.load(std::memory_order_acquire);
        uint32_t read_idx  = g_shm->read_index.load(std::memory_order_relaxed);
        if (write_idx == read_idx) break;  // ring empty

        const AudioFrame& frame = g_shm->mic_frames[read_idx % RING_SIZE];
        size_t need = samples_needed - written;

        if (need >= FRAME_SAMPLES) {
            memcpy(out + written, frame.data, FRAME_BYTES);
            written += FRAME_SAMPLES;
        } else {
            // Partial frame: copy what we need, stash the rest for next call.
            memcpy(out + written, frame.data, need * 2);
            memcpy(g_mic_leftover.samples, frame.data + need,
                   (FRAME_SAMPLES - need) * 2);
            g_mic_leftover.offset = 0;
            g_mic_leftover.valid  = FRAME_SAMPLES - need;
            written += need;
        }

        g_shm->read_index.store((read_idx + 1) % (RING_SIZE * 2),
                                std::memory_order_release);
    }

    return written;
}

static ssize_t hooked_audio_record_read(void* thiz, void* buffer, size_t size, bool blocking) {
    const bool active = g_active.load(std::memory_order_acquire) && g_shm != nullptr;

    if (!active || size < 2) {
        // Server not connected: pass through to the real hardware mic.
        return original_audio_record_read
            ? original_audio_record_read(thiz, buffer, size, blocking)
            : 0;
    }

    // SERVER ACTIVE: completely bypass the HAL — never call original_read so
    // real microphone samples never land in the app's buffer. Anything we
    // don't have in the SHM ring becomes silence, not leaked HW audio.

    g_shm->last_activity.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_relaxed);
    g_shm->audio_capturing.store(true, std::memory_order_relaxed);

    int16_t* out = static_cast<int16_t*>(buffer);
    const size_t samples_needed = size / 2;

    uint32_t app_rate = SAMPLE_RATE;
    if (g_ar_get_sample_rate) app_rate = g_ar_get_sample_rate(thiz);
    if (app_rate == 0) app_rate = SAMPLE_RATE;

    size_t produced = 0;
    if (app_rate == SAMPLE_RATE) {
        produced = pull_mic_samples(out, samples_needed);
    } else {
        g_mic_src.configure(SAMPLE_RATE, app_rate);
        size_t in_needed = (size_t)((uint64_t)samples_needed * SAMPLE_RATE / app_rate) + 2;
        if (in_needed > 9600) in_needed = 9600;
        int16_t staging[9600];
        size_t pulled = pull_mic_samples(staging, in_needed);
        if (pulled > 0) {
            produced = g_mic_src.process(staging, pulled, out, samples_needed);
        }
    }

    // Zero-fill any underrun so no HAL residue is visible.
    if (produced < samples_needed) {
        memset(out + produced, 0, (samples_needed - produced) * 2);
    }

    // Pace the blocking read. If the SHM was empty and the app asked for
    // blocking=true, returning instantly causes CPU-spin polling. Sleep for
    // the natural playback duration so caller cadence stays intact.
    if (blocking && produced == 0) {
        uint32_t us = (uint32_t)((uint64_t)samples_needed * 1000000ULL / app_rate);
        if (us > 40000) us = 40000;       // cap at 40ms to stay responsive
        if (us > 500)   usleep(us);
    }

    return (ssize_t)size;
}

// ─── Speaker capture ───────────────────────────────────────────────────────

static void push_speaker_samples(const int16_t* in, size_t samples) {
    size_t pushed = 0;
    while (pushed < samples) {
        uint32_t write_idx = g_shm->speaker_write_idx.load(std::memory_order_relaxed);
        uint32_t read_idx  = g_shm->speaker_read_idx.load(std::memory_order_acquire);

        // Use mod-2N unsigned-wrapped distance.
        uint32_t depth = (uint32_t)(write_idx - read_idx) % (RING_SIZE * 2);
        if (depth >= RING_SIZE) break;  // ring full, drop rest

        AudioFrame& frame = g_shm->speaker_frames[write_idx % RING_SIZE];
        size_t need = samples - pushed;
        if (need >= FRAME_SAMPLES) {
            memcpy(frame.data, in + pushed, FRAME_BYTES);
            pushed += FRAME_SAMPLES;
        } else {
            memcpy(frame.data, in + pushed, need * 2);
            memset(frame.data + need, 0, (FRAME_SAMPLES - need) * 2);
            pushed = samples;
        }

        frame.timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        frame.flags = 0;
        g_shm->speaker_write_idx.store((write_idx + 1) % (RING_SIZE * 2),
                                       std::memory_order_release);
    }
}

// Flush the speaker accumulator when it has at least one full 48kHz frame.
static void flush_speaker_accum() {
    while (g_spk_accum_n >= FRAME_SAMPLES) {
        push_speaker_samples(g_spk_accum, FRAME_SAMPLES);
        // Shift any overflow samples to the front
        size_t remain = g_spk_accum_n - FRAME_SAMPLES;
        if (remain) memmove(g_spk_accum, g_spk_accum + FRAME_SAMPLES, remain * 2);
        g_spk_accum_n = remain;
    }
}

static ssize_t hooked_audio_track_write(void* thiz, const void* buffer, size_t size, bool blocking) {
    if (g_active.load(std::memory_order_acquire) && g_shm && size >= 2) {
        const int16_t* in = static_cast<const int16_t*>(buffer);
        size_t in_samples = size / 2;

        uint32_t app_rate = SAMPLE_RATE;
        if (g_at_get_sample_rate) app_rate = g_at_get_sample_rate(thiz);
        if (app_rate == 0) app_rate = SAMPLE_RATE;

        if (app_rate == SAMPLE_RATE) {
            push_speaker_samples(in, in_samples);
        } else {
            g_spk_src.configure(app_rate, SAMPLE_RATE);
            // Resample in small input chunks. After each flush, g_spk_accum_n
            // is < FRAME_SAMPLES; with kChunk=512 and worst-case 6× upsample,
            // new output ≤ 3072, and 3072 + 960 = 4032 < SPK_ACCUM_CAP.
            const size_t kChunk = 512;
            size_t done = 0;
            while (done < in_samples) {
                size_t take = std::min(kChunk, in_samples - done);
                size_t space = SPK_ACCUM_CAP - g_spk_accum_n;
                if (space == 0) { flush_speaker_accum(); space = SPK_ACCUM_CAP - g_spk_accum_n; }
                size_t produced = g_spk_src.process(
                    in + done, take,
                    g_spk_accum + g_spk_accum_n, space);
                g_spk_accum_n += produced;
                done += take;
                flush_speaker_accum();
            }
        }
    }
    return original_audio_track_write
        ? original_audio_track_write(thiz, buffer, size, blocking)
        : (ssize_t)size;
}

// ─── App allow-list ────────────────────────────────────────────────────────

static bool app_should_hook(const char* nice_name) {
    if (!nice_name) return false;
    static const char* kAllow[] = {
        "com.audiobridge",
        "com.android.phone",
        "com.android.dialer",
        "com.google.android.dialer",
        "com.android.systemui",
        nullptr,
    };
    for (int i = 0; kAllow[i]; ++i) {
        if (strcmp(nice_name, kAllow[i]) == 0) return true;
    }
    return false;
}

// ─── shadowhook loader + hook installer ────────────────────────────────────

static bool load_shadowhook() {
    void* h = dlopen(kShadowhookPath, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        // Fallback: let the linker try its own search path (in case an
        // enterprising user placed it in /system/lib64).
        h = dlopen("libshadowhook.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!h) {
        LOGE("dlopen libshadowhook.so failed: %s", dlerror());
        return false;
    }
    sh_init          = (shadowhook_init_t)          dlsym(h, "shadowhook_init");
    sh_hook_sym_name = (shadowhook_hook_sym_name_t) dlsym(h, "shadowhook_hook_sym_name");
    sh_get_errno     = (shadowhook_get_errno_t)     dlsym(h, "shadowhook_get_errno");
    sh_to_errmsg     = (shadowhook_to_errmsg_t)     dlsym(h, "shadowhook_to_errmsg");
    sh_dlopen        = (shadowhook_dlopen_t)        dlsym(h, "shadowhook_dlopen");
    sh_dlsym_symtab  = (shadowhook_dlsym_symtab_t)  dlsym(h, "shadowhook_dlsym_symtab");
    if (!sh_init || !sh_hook_sym_name) {
        LOGE("shadowhook symbols missing");
        return false;
    }
    int rc = sh_init(SHADOWHOOK_MODE_SHARED, false);
    if (rc != 0) {
        const char* msg = sh_to_errmsg ? sh_to_errmsg(rc) : "?";
        LOGE("shadowhook_init failed: %d (%s)", rc, msg);
        return false;
    }
    LOGI("shadowhook loaded and initialised");
    return true;
}

// Resolve a symbol in a loaded lib. getSampleRate() is a non-exported member
// accessor — plain dlsym won't find it. shadowhook_dlsym_symtab walks the
// .symtab section which includes local symbols, so it succeeds where dlsym
// fails. Falls back to dlsym for libs where shadowhook can't handle it.
static void* resolve_sym(const char* lib, const char* sym) {
    if (sh_dlopen && sh_dlsym_symtab) {
        void* h = sh_dlopen(lib);
        if (h) {
            void* p = sh_dlsym_symtab(h, sym);
            if (p) return p;
        }
    }
    void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen(lib, RTLD_NOW);
    return h ? dlsym(h, sym) : nullptr;
}

static bool try_hook(const char* lib, const char* const* syms, void* new_fn, void** orig_out) {
    for (int i = 0; syms[i]; ++i) {
        void* stub = sh_hook_sym_name(lib, syms[i], new_fn, orig_out);
        if (stub != nullptr) {
            LOGI("hooked %s", syms[i]);
            return true;
        }
        int err = sh_get_errno ? sh_get_errno() : -1;
        const char* msg = (sh_to_errmsg && err > 0) ? sh_to_errmsg(err) : "?";
        // PENDING (errno==1) means the hook will apply once the lib loads —
        // count that as success too.
        if (err == 1 /* SHADOWHOOK_ERRNO_PENDING */) {
            LOGI("pending hook on %s (lib not loaded yet)", syms[i]);
            return true;
        }
        LOGW("hook %s failed: %d (%s)", syms[i], err, msg);
    }
    return false;
}

static bool install_inline_hooks() {
    if (!sh_hook_sym_name) return false;

    static const char* kReadSyms[] = {
        "_ZN7android11AudioRecord4readEPvmb",  // Android 12+
        "_ZN7android11AudioRecord4readEPvj",   // legacy
        nullptr,
    };
    static const char* kWriteSyms[] = {
        "_ZN7android10AudioTrack5writeEPKvmb", // Android 12+
        "_ZN7android10AudioTrack5writeEPKvj",  // legacy
        nullptr,
    };

    bool r = try_hook("libaudioclient.so", kReadSyms,
                      (void*)hooked_audio_record_read,
                      (void**)&original_audio_record_read);
    bool w = try_hook("libaudioclient.so", kWriteSyms,
                      (void*)hooked_audio_track_write,
                      (void**)&original_audio_track_write);

    // getSampleRate() accessors — plain pointer, no hook.
    g_ar_get_sample_rate = (AudioRecord_getSampleRate_t)resolve_sym(
        "libaudioclient.so", "_ZNK7android11AudioRecord13getSampleRateEv");
    g_at_get_sample_rate = (AudioTrack_getSampleRate_t)resolve_sym(
        "libaudioclient.so", "_ZNK7android10AudioTrack13getSampleRateEv");
    LOGI("getSampleRate: AR=%p AT=%p",
         (void*)g_ar_get_sample_rate, (void*)g_at_get_sample_rate);

    return r || w;
}

// ─── Daemon socket connection ──────────────────────────────────────────────

static void connect_to_daemon_loop() {
    while (!g_active.load()) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { usleep(1000000); continue; }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        strcpy(addr.sun_path + 1, "audio_bridge");
        size_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1
                        + strlen("audio_bridge");

        if (connect(fd, (struct sockaddr*)&addr, addr_len) == 0) {
            send(fd, "GET_SHM_FD", 10, 0);

            struct msghdr msg = {};
            char cbuf[CMSG_SPACE(sizeof(int))];
            memset(cbuf, 0, sizeof(cbuf));
            char iov_buf[2];
            struct iovec io = { .iov_base = iov_buf, .iov_len = sizeof(iov_buf) };
            msg.msg_iov = &io;
            msg.msg_iovlen = 1;
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);

            if (recvmsg(fd, &msg, 0) >= 0) {
                struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
                    int shm_fd = *(int*)CMSG_DATA(cmsg);
                    g_shm = (SharedMemoryLayout*)mmap(
                        nullptr, sizeof(SharedMemoryLayout),
                        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                    if (g_shm != MAP_FAILED) {
                        g_shm->module_active.store(true);
                        g_active.store(true, std::memory_order_release);
                        LOGI("Connected to daemon, SHM at %p", g_shm);
                        send(fd, "STATUS:ACTIVE", 13, 0);
                    } else {
                        g_shm = nullptr;
                    }
                    close(shm_fd);
                }
            }
        }
        close(fd);
        if (!g_active.load()) usleep(1000000);
    }
}

// ─── Zygisk entry point ────────────────────────────────────────────────────

class AudioBridgeModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        const char* nice_name = nullptr;
        if (args && args->nice_name) {
            nice_name = env->GetStringUTFChars(args->nice_name, nullptr);
        }
        bool want_hook = app_should_hook(nice_name);
        if (nice_name) env->ReleaseStringUTFChars(args->nice_name, nice_name);

        if (!want_hook) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            skip = true;
            return;
        }
        // Actual hook install is deferred until postAppSpecialize — at that
        // point libaudioclient.so is more reliably loaded into the app image.
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (skip) return;

        // Force-load libaudioclient.so so hook/symbol resolution can see it.
        // Harmless — any real AudioRecord/AudioTrack use pulls it in anyway.
        void* h = dlopen("libaudioclient.so", RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            LOGW("dlopen(libaudioclient.so) failed: %s", dlerror());
        }

        if (!load_shadowhook()) {
            LOGE("shadowhook unavailable — audio injection disabled");
            return;
        }

        if (!install_inline_hooks()) {
            LOGE("No inline hooks installed — audio injection disabled");
        }

        std::thread(connect_to_daemon_loop).detach();
    }

private:
    zygisk::Api* api = nullptr;
    JNIEnv*      env = nullptr;
    bool         skip = false;
};

REGISTER_ZYGISK_MODULE(AudioBridgeModule)
