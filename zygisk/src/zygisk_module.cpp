#include <jni.h>
#include <string>
#include <atomic>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <android/log.h>
#include <chrono>
#include <string.h>
#include <unistd.h>
#include "zygisk.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AudioBridge-Zygisk", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "AudioBridge-Zygisk", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AudioBridge-Zygisk", __VA_ARGS__)

static const char* SOCKET_PATH = "/data/local/tmp/audio_bridge.sock";
static const int SAMPLE_RATE = 48000;
static const int FRAME_SAMPLES = 960;

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
    AudioFrame mic_frames[64];
    AudioFrame speaker_frames[64];
};

static SharedMemoryLayout* g_shm = nullptr;
static std::atomic<bool> g_active{false};

// AudioTrack hook - captures speaker output
static ssize_t (*original_audio_track_write)(void*, const void*, size_t) = nullptr;

static ssize_t hooked_audio_track_write(void* thiz, const void* buffer, size_t size) {
    if (g_active && g_shm && size >= FRAME_SAMPLES * 2) {
        uint32_t write_idx = g_shm->speaker_write_idx.load();
        uint32_t read_idx = g_shm->speaker_read_idx.load();

        if ((write_idx - read_idx) < 60) {
            AudioFrame& frame = g_shm->speaker_frames[write_idx % 64];
            memcpy(frame.data, buffer, FRAME_SAMPLES * 2);
            frame.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            g_shm->speaker_write_idx.store((write_idx + 1) % (64 * 2));
        }
    }

    return original_audio_track_write(thiz, buffer, size);
}

// AudioRecord hook - injects virtual mic data
static ssize_t (*original_audio_record_read)(void*, void*, size_t) = nullptr;

static ssize_t hooked_audio_record_read(void* thiz, void* buffer, size_t size) {
    if (g_active && g_shm && size >= FRAME_SAMPLES * 2) {
        uint32_t write_idx = g_shm->write_index.load();
        uint32_t read_idx = g_shm->read_index.load();

        if (write_idx != read_idx) {
            AudioFrame& frame = g_shm->mic_frames[read_idx % 64];
            memcpy(buffer, frame.data, FRAME_SAMPLES * 2);
            g_shm->read_index.store((read_idx + 1) % (64 * 2));
            return FRAME_SAMPLES * 2;
        }
    }

    return original_audio_record_read ?
           original_audio_record_read(thiz, buffer, size) : 0;
}

class AudioBridgeModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
        LOGI("AudioBridge Zygisk module loaded");
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // ALWAYS register hooks, even if daemon isn't running yet!
        dev_t dev = 0;
        ino_t ino = 0;
        get_lib_dev_ino("libaudioclient.so", &dev, &ino);
        
        if (dev != 0 && ino != 0) {
            // Hook AudioTrack::write
            api->pltHookRegister(dev, ino,
                "_ZN7android10AudioTrack5writeEPKvj",
                (void*)hooked_audio_track_write,
                (void**)&original_audio_track_write);

            // Hook AudioRecord::read
            api->pltHookRegister(dev, ino,
                "_ZN7android10AudioRecord4readEPvj",
                (void*)hooked_audio_record_read,
                (void**)&original_audio_record_read);

            LOGI("Audio hooks registered for libaudioclient.so (dev:%llu, ino:%llu)", 
                 (unsigned long long)dev, (unsigned long long)ino);
        } else {
            LOGW("Could not find libaudioclient.so in memory map to hook");
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        // Run connection loop in a background thread so we don't block app startup
        std::thread([this]() {
            while (!g_active) {
                int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd >= 0) {
                    struct sockaddr_un addr{};
                    addr.sun_family = AF_UNIX;
                    addr.sun_path[0] = '\0';
                    strcpy(addr.sun_path + 1, "audio_bridge");
                    size_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen("audio_bridge");

                    if (connect(fd, (struct sockaddr*)&addr, addr_len) == 0) {
                        send(fd, "GET_SHM_FD", 10, 0);

                        struct msghdr msg = {};
                        char buf[CMSG_SPACE(sizeof(int))];
                        memset(buf, 0, sizeof(buf));

                        char iov_buf[2];
                        struct iovec io = { .iov_base = iov_buf, .iov_len = sizeof(iov_buf) };
                        msg.msg_iov = &io;
                        msg.msg_iovlen = 1;
                        msg.msg_control = buf;
                        msg.msg_controllen = sizeof(buf);

                        if (recvmsg(fd, &msg, 0) >= 0) {
                            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
                            if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
                                int shm_fd = *(int*)CMSG_DATA(cmsg);

                                g_shm = (SharedMemoryLayout*)mmap(nullptr, sizeof(SharedMemoryLayout),
                                                                  PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                                if (g_shm != MAP_FAILED) {
                                    g_shm->module_active = true;
                                    g_active = true;
                                    LOGI("Connected to audio bridge, SHM at %p", g_shm);
                                    send(fd, "STATUS:ACTIVE", 13, 0);
                                }
                                close(shm_fd);
                            }
                        }
                    }
                    close(fd);
                }
                
                if (!g_active) {
                    usleep(1000000); // Wait 1 second before retrying
                }
            }
        }).detach();
    }

private:
    void get_lib_dev_ino(const char* libname, dev_t* dev, ino_t* ino) {
        *dev = 0;
        *ino = 0;
        FILE* fp = fopen("/proc/self/maps", "r");
        if (!fp) return;
        
        char line[1024];
        char path[512] = {0};
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, libname)) {
                char* p = strchr(line, '/');
                if (p) {
                    char* end = strchr(p, '\n');
                    if (end) *end = 0;
                    strncpy(path, p, sizeof(path) - 1);
                    break;
                }
            }
        }
        fclose(fp);
        
        if (path[0]) {
            struct stat st;
            if (stat(path, &st) == 0) {
                *dev = st.st_dev;
                *ino = st.st_ino;
            }
        }
    }

    zygisk::Api* api = nullptr;
    JNIEnv* env = nullptr;
};

REGISTER_ZYGISK_MODULE(AudioBridgeModule)
