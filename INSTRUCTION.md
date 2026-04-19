# Complete Audio Bridge System with Telephony & SMS Control

## 📁 Project Structure

```
audio-bridge/
├── README.md
├── LICENSE
├── build.sh
├── CMakeLists.txt
├── Android.mk
├── Application.mk
├── server/
│   ├── server_example.py
│   ├── server_example.js
│   ├── protocol.md
│   └── test_client.py
├── jni/
│   ├── audio_bridge.cpp
│   ├── audio_bridge.h
│   ├── opus_wrapper.cpp
│   ├── opus_wrapper.h
│   ├── jni_bridge.cpp
│   └── Android.mk
├── java/
│   └── com/
│       └── audiobridge/
│           ├── TelephonyHelper.java
│           ├── AudioBridgeService.java
│           └── BootReceiver.java
├── libs/
│   ├── arm64-v8a/
│   │   ├── libopus.so
│   │   └── libtinyalsa.so
│   └── armeabi-v7a/
│       ├── libopus.so
│       └── libtinyalsa.so
├── zygisk/
│   ├── module/
│   │   ├── module.prop
│   │   ├── post-fs-data.sh
│   │   └── service.sh
│   └── src/
│       ├── zygisk_module.cpp
│       └── Android.mk
├── config/
│   ├── audio_bridge.conf
│   └── audio_policy.conf
└── scripts/
    ├── install.sh
    ├── start.sh
    ├── stop.sh
    └── uninstall.sh
```

## 📄 Full Source Code

### 1. **Main Native Code** - `jni/audio_bridge.cpp`

```cpp
/**
 * Audio Bridge - Complete System with Telephony & SMS Control
 * Version: 3.0
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <tinyalsa/asoundlib.h>
#include <opus/opus.h>
#include <vector>
#include <deque>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <map>
#include <condition_variable>
#include <sstream>
#include <iomanip>

// ──────────────────────────────────────────────────────────────────────────
// Configuration Constants
// ──────────────────────────────────────────────────────────────────────────

#define VERSION_MAJOR 3
#define VERSION_MINOR 0
#define VERSION_PATCH 0

static const char* g_host         = nullptr;
static int         g_port         = 59100;
static const char* g_socket_path  = "/data/local/tmp/audio_bridge.sock";
static const char* g_pid_file     = "/data/local/tmp/audio_bridge.pid";
static const char* g_shm_path     = "/audio_bridge_shm";

static const int SAMPLE_RATE      = 48000;
static const int CHANNELS         = 1;
static const int FRAME_MS         = 20;
static const int FRAME_SAMPLES    = (SAMPLE_RATE * FRAME_MS / 1000);
static const int FRAME_BYTES      = FRAME_SAMPLES * sizeof(int16_t);
static const int MAX_PKT          = 4000;
static const int JITTER_FRAMES    = 6;
static const int SHM_RING_SIZE    = 64;
static const int SHM_SIZE         = 1024 * 1024;

// Frame Types (Multiplex Protocol)
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

// Call States
enum CallState : int {
    CALL_IDLE     = 0,
    CALL_RINGING  = 1,
    CALL_OFFHOOK  = 2,
    CALL_DIALING  = 3,
    CALL_HOLDING  = 4
};

// ──────────────────────────────────────────────────────────────────────────
// Global State
// ──────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_audio_active{false};
static std::atomic<int>  g_call_state{CALL_IDLE};

static int         g_shm_fd       = -1;
static void*       g_shm_ptr      = nullptr;
static JavaVM*     g_jvm          = nullptr;
static jclass      g_helper_class = nullptr;
static jobject     g_helper_obj   = nullptr;

static std::mutex              g_status_mutex;
static std::condition_variable g_status_cv;
static std::queue<std::string> g_status_queue;
static std::atomic<bool>       g_status_pending{false};

static std::mutex              g_call_mutex;
static std::string             g_current_number;
static std::map<std::string, std::string> g_active_calls;

static std::mutex              g_sms_mutex;
static std::map<std::string, Json::Value> g_sms_tracking;

static std::mutex              g_log_mutex;
static FILE*                   g_log_file = nullptr;

// ──────────────────────────────────────────────────────────────────────────
// Shared Memory Layout (Must match Zygisk module)
// ──────────────────────────────────────────────────────────────────────────

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
// JSON Helper (Minimal implementation without external lib)
// ──────────────────────────────────────────────────────────────────────────

class SimpleJson {
public:
    enum Type { OBJECT, ARRAY, STRING, NUMBER, BOOLEAN, NULL_TYPE };
    
    Type type;
    std::string string_value;
    double number_value;
    bool bool_value;
    std::map<std::string, SimpleJson> object_value;
    std::vector<SimpleJson> array_value;
    
    SimpleJson() : type(NULL_TYPE) {}
    SimpleJson(const std::string& str) : type(STRING), string_value(str) {}
    SimpleJson(double num) : type(NUMBER), number_value(num) {}
    SimpleJson(bool b) : type(BOOLEAN), bool_value(b) {}
    
    std::string toString() const {
        std::ostringstream oss;
        serialize(oss);
        return oss.str();
    }
    
    void serialize(std::ostringstream& oss) const {
        switch(type) {
            case OBJECT:
                oss << "{";
                {
                    bool first = true;
                    for(const auto& p : object_value) {
                        if(!first) oss << ",";
                        oss << "\"" << escape(p.first) << "\":";
                        p.second.serialize(oss);
                        first = false;
                    }
                }
                oss << "}";
                break;
            case ARRAY:
                oss << "[";
                {
                    bool first = true;
                    for(const auto& v : array_value) {
                        if(!first) oss << ",";
                        v.serialize(oss);
                        first = false;
                    }
                }
                oss << "]";
                break;
            case STRING:
                oss << "\"" << escape(string_value) << "\"";
                break;
            case NUMBER:
                oss << std::fixed << number_value;
                break;
            case BOOLEAN:
                oss << (bool_value ? "true" : "false");
                break;
            case NULL_TYPE:
                oss << "null";
                break;
        }
    }
    
    static std::string escape(const std::string& s) {
        std::string result;
        for(char c : s) {
            switch(c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c;
            }
        }
        return result;
    }
    
    static SimpleJson parse(const std::string& json) {
        // Simplified parser for demo - use jsoncpp in production
        SimpleJson obj;
        obj.type = OBJECT;
        
        // Very basic parsing - just enough for our protocol
        size_t pos = json.find("\"command\"");
        if(pos != std::string::npos) {
            pos = json.find(":", pos);
            if(pos != std::string::npos) {
                size_t start = json.find("\"", pos + 1);
                size_t end = json.find("\"", start + 1);
                if(start != std::string::npos && end != std::string::npos) {
                    obj.object_value["command"] = SimpleJson(json.substr(start + 1, end - start - 1));
                }
            }
        }
        
        pos = json.find("\"number\"");
        if(pos != std::string::npos) {
            pos = json.find(":", pos);
            if(pos != std::string::npos) {
                size_t start = json.find("\"", pos + 1);
                size_t end = json.find("\"", start + 1);
                if(start != std::string::npos && end != std::string::npos) {
                    obj.object_value["number"] = SimpleJson(json.substr(start + 1, end - start - 1));
                }
            }
        }
        
        pos = json.find("\"message\"");
        if(pos != std::string::npos) {
            pos = json.find(":", pos);
            if(pos != std::string::npos) {
                size_t start = json.find("\"", pos + 1);
                size_t end = json.find("\"", start + 1);
                if(start != std::string::npos && end != std::string::npos) {
                    obj.object_value["message"] = SimpleJson(json.substr(start + 1, end - start - 1));
                }
            }
        }
        
        return obj;
    }
    
    std::string getString(const std::string& key, const std::string& def = "") const {
        auto it = object_value.find(key);
        return (it != object_value.end() && it->second.type == STRING) ? 
               it->second.string_value : def;
    }
    
    bool hasKey(const std::string& key) const {
        return object_value.find(key) != object_value.end();
    }
};

// ──────────────────────────────────────────────────────────────────────────
// Logging Utilities
// ──────────────────────────────────────────────────────────────────────────

#define LOG_TAG "AudioBridge"

static void log_init() {
    g_log_file = fopen("/data/local/tmp/audio_bridge.log", "a");
    if(g_log_file) {
        time_t now = time(nullptr);
        fprintf(g_log_file, "\n=== Audio Bridge v%d.%d.%d started at %s ===\n",
                VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, ctime(&now));
        fflush(g_log_file);
    }
}

static void log_write(const char* level, const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);
    
    va_list args;
    va_start(args, fmt);
    
    // Console output
    fprintf(stderr, "[%s] [%s] ", time_buf, level);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    
    // File output
    if(g_log_file) {
        fprintf(g_log_file, "[%s] [%s] ", time_buf, level);
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
    
    va_end(args);
}

#define LOGI(...) log_write("INFO", __VA_ARGS__)
#define LOGW(...) log_write("WARN", __VA_ARGS__)
#define LOGE(...) log_write("ERROR", __VA_ARGS__)
#define LOGD(...) log_write("DEBUG", __VA_ARGS__)

// ──────────────────────────────────────────────────────────────────────────
// System Helpers
// ──────────────────────────────────────────────────────────────────────────

static std::string get_prop(const char* key, const char* def = "") {
    char val[PROP_VALUE_MAX] = {};
    __system_property_get(key, val);
    return val[0] ? val : def;
}

static std::string get_device_id() {
    const char* ID_FILE = "/data/local/tmp/audio_bridge_id";
    FILE* f = fopen(ID_FILE, "r");
    static char buf[64];
    
    if(f && fscanf(f, "%63s", buf) == 1 && buf[0]) { 
        fclose(f); 
        return buf; 
    }
    if(f) fclose(f);
    
    std::string s = get_prop("ro.serialno");
    if(s.empty()) {
        srand(time(nullptr) ^ getpid());
        snprintf(buf, sizeof(buf), "%08x%08x", rand(), rand());
        s = buf;
    }
    
    if((f = fopen(ID_FILE, "w"))) {
        fprintf(f, "%s\n", s.c_str());
        fclose(f);
        chmod(ID_FILE, 0600);
    }
    return s;
}

static void write_pid_file() {
    FILE* f = fopen(g_pid_file, "w");
    if(f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

static void remove_pid_file() {
    unlink(g_pid_file);
}

// ──────────────────────────────────────────────────────────────────────────
// Network Utilities
// ──────────────────────────────────────────────────────────────────────────

static bool send_all(int fd, const void* data, size_t len) {
    const auto* p = (const uint8_t*)data;
    while(len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool recv_all(int fd, void* data, size_t len) {
    auto* p = (uint8_t*)data;
    while(len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool send_frame(int fd, uint8_t type, const void* data, uint32_t len) {
    uint8_t hdr[5];
    hdr[0] = type;
    hdr[1] = (len >> 24) & 0xFF;
    hdr[2] = (len >> 16) & 0xFF;
    hdr[3] = (len >>  8) & 0xFF;
    hdr[4] = (len >>  0) & 0xFF;
    return send_all(fd, hdr, 5) && send_all(fd, data, len);
}

static bool send_json(int fd, uint8_t type, const SimpleJson& json) {
    std::string str = json.toString();
    return send_frame(fd, type, str.c_str(), str.length());
}

static int tcp_connect(const char* host, int port) {
    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char p[8];
    snprintf(p, sizeof(p), "%d", port);
    
    if(getaddrinfo(host, p, &hints, &res) != 0) return -1;
    
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if(fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    
    int keepidle = 30, keepintvl = 5, keepcnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
    
    struct timeval tv{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if(connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    
    tv = {0, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    freeaddrinfo(res);
    return fd;
}

static bool handshake(int fd) {
    SimpleJson reg;
    reg.type = SimpleJson::OBJECT;
    reg.object_value["type"] = SimpleJson("register");
    reg.object_value["name"] = SimpleJson(get_prop("ro.product.model", "Android"));
    reg.object_value["brand"] = SimpleJson(get_prop("ro.product.brand", ""));
    reg.object_value["android"] = SimpleJson(get_prop("ro.build.version.release", ""));
    reg.object_value["id"] = SimpleJson(get_device_id());
    reg.object_value["mode"] = SimpleJson("full_control");
    reg.object_value["version"] = SimpleJson(std::to_string(VERSION_MAJOR) + "." + 
                                            std::to_string(VERSION_MINOR) + "." + 
                                            std::to_string(VERSION_PATCH));
    reg.object_value["features"] = SimpleJson::ARRAY;
    reg.object_value["features"].array_value.push_back(SimpleJson("audio"));
    reg.object_value["features"].array_value.push_back(SimpleJson("call_control"));
    reg.object_value["features"].array_value.push_back(SimpleJson("sms"));
    
    std::string str = reg.toString() + "\n";
    
    if(!send_all(fd, str.c_str(), str.size())) return false;
    
    std::string line;
    char c;
    while(true) {
        if(recv(fd, &c, 1, 0) != 1) return false;
        if(c == '\n') break;
        line += c;
        if(line.size() > 512) return false;
    }
    
    LOGI("Handshake response: %s", line.c_str());
    return line.find("\"ok\"") != std::string::npos;
}

// ──────────────────────────────────────────────────────────────────────────
// Shared Memory
// ──────────────────────────────────────────────────────────────────────────

static bool setup_shared_memory() {
    g_shm_fd = memfd_create(g_shm_path, MFD_CLOEXEC);
    if(g_shm_fd < 0) {
        g_shm_fd = open("/dev/ashmem", O_RDWR);
        if(g_shm_fd < 0) {
            LOGE("Failed to create shared memory: %s", strerror(errno));
            return false;
        }
    }
    
    if(ftruncate(g_shm_fd, SHM_SIZE) < 0) {
        LOGE("ftruncate failed: %s", strerror(errno));
        close(g_shm_fd);
        return false;
    }
    
    g_shm_ptr = mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if(g_shm_ptr == MAP_FAILED) {
        LOGE("mmap failed: %s", strerror(errno));
        close(g_shm_fd);
        return false;
    }
    
    auto* layout = (SharedMemoryLayout*)g_shm_ptr;
    memset(layout, 0, SHM_SIZE);
    
    LOGI("Shared memory initialized at %p, fd=%d", g_shm_ptr, g_shm_fd);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// JNI Bridge
// ──────────────────────────────────────────────────────────────────────────

static void init_jni() {
    JNI_GetCreatedJavaVMs(&g_jvm, 1, nullptr);
    if(!g_jvm) {
        LOGW("No Java VM found - call/SMS features disabled");
        return;
    }
    
    JNIEnv* env;
    if(g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("Failed to attach to JVM");
        return;
    }
    
    g_helper_class = env->FindClass("com/audiobridge/TelephonyHelper");
    if(!g_helper_class) {
        LOGW("TelephonyHelper class not found");
        g_jvm->DetachCurrentThread();
        return;
    }
    
    jmethodID getInstance = env->GetStaticMethodID(g_helper_class, "getInstance", 
                                                   "()Lcom/audiobridge/TelephonyHelper;");
    if(getInstance) {
        jobject obj = env->CallStaticObjectMethod(g_helper_class, getInstance);
        if(obj) {
            g_helper_obj = env->NewGlobalRef(obj);
            LOGI("TelephonyHelper initialized");
        }
    }
    
    g_jvm->DetachCurrentThread();
}

static void jni_place_call(const std::string& number) {
    if(!g_helper_obj) return;
    
    JNIEnv* env;
    if(g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    
    jclass cls = env->GetObjectClass(g_helper_obj);
    jmethodID method = env->GetMethodID(cls, "placeCall", "(Ljava/lang/String;)V");
    if(method) {
        jstring jNumber = env->NewStringUTF(number.c_str());
        env->CallVoidMethod(g_helper_obj, method, jNumber);
        env->DeleteLocalRef(jNumber);
    }
    
    g_jvm->DetachCurrentThread();
}

static void jni_end_call() {
    if(!g_helper_obj) return;
    
    JNIEnv* env;
    if(g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    
    jclass cls = env->GetObjectClass(g_helper_obj);
    jmethodID method = env->GetMethodID(cls, "endCall", "()V");
    if(method) {
        env->CallVoidMethod(g_helper_obj, method);
    }
    
    g_jvm->DetachCurrentThread();
}

static void jni_answer_call() {
    if(!g_helper_obj) return;
    
    JNIEnv* env;
    if(g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    
    jclass cls = env->GetObjectClass(g_helper_obj);
    jmethodID method = env->GetMethodID(cls, "answerCall", "()V");
    if(method) {
        env->CallVoidMethod(g_helper_obj, method);
    }
    
    g_jvm->DetachCurrentThread();
}

static std::string jni_send_sms(const std::string& number, const std::string& message) {
    if(!g_helper_obj) return "";
    
    JNIEnv* env;
    if(g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return "";
    
    jclass cls = env->GetObjectClass(g_helper_obj);
    jmethodID method = env->GetMethodID(cls, "sendSMS", 
                                        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    std::string result;
    if(method) {
        jstring jNumber = env->NewStringUTF(number.c_str());
        jstring jMessage = env->NewStringUTF(message.c_str());
        jstring jResult = (jstring)env->CallObjectMethod(g_helper_obj, method, jNumber, jMessage);
        
        if(jResult) {
            const char* str = env->GetStringUTFChars(jResult, nullptr);
            result = str;
            env->ReleaseStringUTFChars(jResult, str);
        }
        
        env->DeleteLocalRef(jNumber);
        env->DeleteLocalRef(jMessage);
    }
    
    g_jvm->DetachCurrentThread();
    return result;
}

// ──────────────────────────────────────────────────────────────────────────
// JNI Callbacks (Called from Java)
// ──────────────────────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnCallStateChanged(
    JNIEnv* env, jobject thiz, jint state, jstring number) {
    
    g_call_state = state;
    
    const char* numStr = env->GetStringUTFChars(number, nullptr);
    std::string num = numStr ? numStr : "";
    env->ReleaseStringUTFChars(number, numStr);
    
    {
        std::lock_guard<std::mutex> lk(g_call_mutex);
        g_current_number = num;
    }
    
    SimpleJson status;
    status.type = SimpleJson::OBJECT;
    status.object_value["type"] = SimpleJson("call_status");
    status.object_value["state"] = SimpleJson((double)state);
    status.object_value["state_name"] = SimpleJson(
        state == CALL_IDLE ? "IDLE" :
        state == CALL_RINGING ? "RINGING" :
        state == CALL_OFFHOOK ? "OFFHOOK" : "UNKNOWN");
    status.object_value["number"] = SimpleJson(num);
    status.object_value["timestamp"] = SimpleJson((double)time(nullptr));
    
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_status_queue.push(status.toString());
        g_status_pending = true;
    }
    g_status_cv.notify_one();
    
    LOGI("Call state: %d, number: %s", state, num.c_str());
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnCallWaiting(
    JNIEnv* env, jobject thiz, jstring incomingNumber, jstring currentNumber) {
    
    const char* incoming = env->GetStringUTFChars(incomingNumber, nullptr);
    const char* current = env->GetStringUTFChars(currentNumber, nullptr);
    
    SimpleJson status;
    status.type = SimpleJson::OBJECT;
    status.object_value["type"] = SimpleJson("call_waiting");
    status.object_value["incoming_number"] = SimpleJson(incoming ? incoming : "");
    status.object_value["active_number"] = SimpleJson(current ? current : "");
    status.object_value["timestamp"] = SimpleJson((double)time(nullptr));
    status.object_value["available_actions"] = SimpleJson::ARRAY;
    status.object_value["available_actions"].array_value.push_back(SimpleJson("hold_and_answer"));
    status.object_value["available_actions"].array_value.push_back(SimpleJson("hangup_and_answer"));
    status.object_value["available_actions"].array_value.push_back(SimpleJson("ignore"));
    
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_status_queue.push(status.toString());
        g_status_pending = true;
    }
    g_status_cv.notify_one();
    
    env->ReleaseStringUTFChars(incomingNumber, incoming);
    env->ReleaseStringUTFChars(currentNumber, current);
    
    LOGI("Call waiting: %s (active: %s)", incoming ? incoming : "?", current ? current : "?");
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnSMSSent(
    JNIEnv* env, jobject thiz, jstring messageId, jint resultCode) {
    
    const char* id = env->GetStringUTFChars(messageId, nullptr);
    
    SimpleJson status;
    status.type = SimpleJson::OBJECT;
    status.object_value["type"] = SimpleJson("sms_status");
    status.object_value["message_id"] = SimpleJson(id ? id : "");
    status.object_value["result"] = SimpleJson(resultCode == -1 ? "sent" : "failed");
    status.object_value["result_code"] = SimpleJson((double)resultCode);
    status.object_value["timestamp"] = SimpleJson((double)time(nullptr));
    
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_status_queue.push(status.toString());
        g_status_pending = true;
    }
    g_status_cv.notify_one();
    
    env->ReleaseStringUTFChars(messageId, id);
    
    LOGI("SMS sent: %s, result=%d", id ? id : "?", resultCode);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnSMSDelivered(
    JNIEnv* env, jobject thiz, jstring messageId) {
    
    const char* id = env->GetStringUTFChars(messageId, nullptr);
    
    SimpleJson status;
    status.type = SimpleJson::OBJECT;
    status.object_value["type"] = SimpleJson("sms_status");
    status.object_value["message_id"] = SimpleJson(id ? id : "");
    status.object_value["status"] = SimpleJson("delivered");
    status.object_value["timestamp"] = SimpleJson((double)time(nullptr));
    
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_status_queue.push(status.toString());
        g_status_pending = true;
    }
    g_status_cv.notify_one();
    
    env->ReleaseStringUTFChars(messageId, id);
    
    LOGI("SMS delivered: %s", id ? id : "?");
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnSMSReceived(
    JNIEnv* env, jobject thiz, jstring sender, jstring message, jlong timestamp) {
    
    const char* from = env->GetStringUTFChars(sender, nullptr);
    const char* msg = env->GetStringUTFChars(message, nullptr);
    
    SimpleJson status;
    status.type = SimpleJson::OBJECT;
    status.object_value["type"] = SimpleJson("sms_received");
    status.object_value["sender"] = SimpleJson(from ? from : "");
    status.object_value["message"] = SimpleJson(msg ? msg : "");
    status.object_value["timestamp"] = SimpleJson((double)timestamp);
    
    {
        std::lock_guard<std::mutex> lk(g_status_mutex);
        g_status_queue.push(status.toString());
        g_status_pending = true;
    }
    g_status_cv.notify_one();
    
    env->ReleaseStringUTFChars(sender, from);
    env->ReleaseStringUTFChars(message, msg);
    
    LOGI("SMS received from %s", from ? from : "?");
}

} // extern "C"

// ──────────────────────────────────────────────────────────────────────────
// Thread Functions
// ──────────────────────────────────────────────────────────────────────────

static void status_sender_thread(int fd) {
    LOGI("Status sender started");
    
    while(g_running && g_connected) {
        std::string json_str;
        bool has_status = false;
        
        {
            std::unique_lock<std::mutex> lk(g_status_mutex);
            
            if(!g_status_pending) {
                g_status_cv.wait_for(lk, std::chrono::milliseconds(100));
            }
            
            if(!g_status_queue.empty()) {
                json_str = g_status_queue.front();
                g_status_queue.pop();
                has_status = true;
                g_status_pending = !g_status_queue.empty();
            }
        }
        
        if(has_status && g_connected) {
            if(send_frame(fd, T_CALL_STATUS, json_str.c_str(), json_str.length())) {
                LOGD("Status sent: %s", json_str.substr(0, 100).c_str());
            } else {
                LOGE("Failed to send status, re-queueing");
                std::lock_guard<std::mutex> lk(g_status_mutex);
                g_status_queue.push(json_str);
                g_status_pending = true;
            }
        }
    }
    
    LOGI("Status sender exited");
}

static void capture_speaker_thread(int fd) {
    auto* layout = (SharedMemoryLayout*)g_shm_ptr;
    
    int err;
    OpusEncoder* enc = opus_encoder_create(SAMPLE_RATE, CHANNELS, 
                                           OPUS_APPLICATION_AUDIO, &err);
    if(!enc) {
        LOGE("Failed to create Opus encoder: %d", err);
        return;
    }
    
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
    
    std::vector<uint8_t> pkt(MAX_PKT);
    uint64_t frames_sent = 0;
    
    while(g_running && g_connected) {
        uint32_t write_idx = layout->speaker_write_idx.load(std::memory_order_acquire);
        uint32_t read_idx = layout->speaker_read_idx.load(std::memory_order_acquire);
        
        if(write_idx == read_idx) {
            usleep(5000);
            continue;
        }
        
        AudioFrame& frame = layout->speaker_frames[read_idx % SHM_RING_SIZE];
        
        opus_int32 len = opus_encode(enc, frame.data, FRAME_SAMPLES, 
                                     pkt.data(), MAX_PKT);
        if(len > 0) {
            if(!send_frame(fd, T_SPEAKER, pkt.data(), (uint32_t)len)) {
                break;
            }
            frames_sent++;
        }
        
        layout->speaker_read_idx.store((read_idx + 1) % (SHM_RING_SIZE * 2), 
                                       std::memory_order_release);
        
        if(frames_sent % 50 == 0) {
            LOGD("Speaker: %llu frames sent", (unsigned long long)frames_sent);
        }
    }
    
    opus_encoder_destroy(enc);
    LOGI("Speaker capture exited (frames sent: %llu)", (unsigned long long)frames_sent);
}

static void receive_virtual_mic_thread(int fd) {
    auto* layout = (SharedMemoryLayout*)g_shm_ptr;
    
    int err;
    OpusDecoder* dec = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if(!dec) {
        LOGE("Failed to create Opus decoder: %d", err);
        return;
    }
    
    std::vector<uint8_t> pkt(MAX_PKT);
    uint8_t hdr[5];
    uint64_t frames_received = 0;
    
    while(g_running && g_connected) {
        if(!recv_all(fd, hdr, 5)) break;
        
        uint8_t type = hdr[0];
        uint32_t len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                       ((uint32_t)hdr[3] <<  8) | ((uint32_t)hdr[4]);
        
        if(len == 0 || len > MAX_PKT) break;
        if(!recv_all(fd, pkt.data(), len)) break;
        
        // Handle Control Messages
        if(type == T_CONTROL) {
            pkt.data()[len] = '\0';
            std::string json_str((char*)pkt.data(), len);
            
            SimpleJson root = SimpleJson::parse(json_str);
            std::string cmd = root.getString("command");
            
            LOGI("Control command: %s", cmd.c_str());
            
            if(cmd == "dial") {
                std::string number = root.getString("number");
                if(!number.empty()) {
                    jni_place_call(number);
                }
            } else if(cmd == "hangup" || cmd == "end_call") {
                jni_end_call();
            } else if(cmd == "answer") {
                jni_answer_call();
            } else if(cmd == "send_sms") {
                std::string number = root.getString("number");
                std::string message = root.getString("message");
                if(!number.empty() && !message.empty()) {
                    std::string msg_id = jni_send_sms(number, message);
                    LOGI("SMS queued: %s", msg_id.c_str());
                }
            } else if(cmd == "ping") {
                SimpleJson pong;
                pong.type = SimpleJson::OBJECT;
                pong.object_value["type"] = SimpleJson("pong");
                pong.object_value["timestamp"] = SimpleJson((double)time(nullptr));
                send_json(fd, T_PONG, pong);
            }
            
            continue;
        }
        
        // Handle Audio (Virtual Mic)
        if(type != T_VIRTUAL_MIC) continue;
        
        uint32_t write_idx = layout->write_index.load(std::memory_order_acquire);
        uint32_t read_idx = layout->read_index.load(std::memory_order_acquire);
        
        if((write_idx - read_idx) >= SHM_RING_SIZE) {
            LOGW("Mic buffer full, dropping frame");
            continue;
        }
        
        AudioFrame& frame = layout->mic_frames[write_idx % SHM_RING_SIZE];
        
        int n = opus_decode(dec, pkt.data(), (opus_int32)len,
                           frame.data, FRAME_SAMPLES, 0);
        if(n < 0) {
            n = opus_decode(dec, nullptr, 0, frame.data, FRAME_SAMPLES, 0);
        }
        
        if(n > 0) {
            frame.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            frame.flags = 0;
            layout->write_index.store((write_idx + 1) % (SHM_RING_SIZE * 2),
                                      std::memory_order_release);
            frames_received++;
        }
    }
    
    opus_decoder_destroy(dec);
    LOGI("Virtual mic receiver exited (frames: %llu)", (unsigned long long)frames_received);
}

static void unix_socket_server_thread() {
    unlink(g_socket_path);
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(server_fd < 0) {
        LOGE("Unix socket creation failed: %s", strerror(errno));
        return;
    }
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1);
    
    if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Unix socket bind failed: %s", strerror(errno));
        close(server_fd);
        return;
    }
    
    chmod(g_socket_path, 0666);
    
    if(listen(server_fd, 5) < 0) {
        LOGE("Unix socket listen failed: %s", strerror(errno));
        close(server_fd);
        return;
    }
    
    LOGI("Unix socket listening on %s", g_socket_path);
    
    auto* layout = (SharedMemoryLayout*)g_shm_ptr;
    
    while(g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        
        struct timeval tv{1, 0};
        int ret = select(server_fd + 1, &fds, nullptr, nullptr, &tv);
        if(ret < 0) break;
        if(ret == 0) continue;
        
        int client_fd = accept(server_fd, nullptr, nullptr);
        if(client_fd < 0) continue;
        
        char cmd[256];
        ssize_t n = recv(client_fd, cmd, sizeof(cmd) - 1, 0);
        if(n > 0) {
            cmd[n] = '\0';
            
            if(strcmp(cmd, "GET_SHM_FD") == 0) {
                struct msghdr msg = {};
                char buf[CMSG_SPACE(sizeof(int))];
                memset(buf, 0, sizeof(buf));
                
                struct iovec io = { .iov_base = (void*)"OK", .iov_len = 2 };
                msg.msg_iov = &io;
                msg.msg_iovlen = 1;
                msg.msg_control = buf;
                msg.msg_controllen = sizeof(buf);
                
                struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                *(int*)CMSG_DATA(cmsg) = g_shm_fd;
                
                sendmsg(client_fd, &msg, 0);
                layout->module_active = true;
                LOGI("Shared memory FD sent to Zygisk module");
            } else if(strcmp(cmd, "PING") == 0) {
                send(client_fd, "PONG", 4, 0);
            }
        }
        
        close(client_fd);
    }
    
    close(server_fd);
    unlink(g_socket_path);
    LOGI("Unix socket server exited");
}

// ──────────────────────────────────────────────────────────────────────────
// Signal Handlers
// ──────────────────────────────────────────────────────────────────────────

static void signal_handler(int sig) {
    LOGI("Received signal %d, shutting down...", sig);
    g_running = false;
    g_connected = false;
}

// ──────────────────────────────────────────────────────────────────────────
// Main
// ──────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Parse arguments
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--host") && i+1 < argc) g_host = argv[++i];
        else if(!strcmp(argv[i], "--port") && i+1 < argc) g_port = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--socket") && i+1 < argc) g_socket_path = argv[++i];
        else if(!strcmp(argv[i], "--daemon")) {
            // Daemonize
            if(fork() > 0) exit(0);
            setsid();
            fclose(stdin);
            fclose(stdout);
        }
    }
    
    // Load config from file if host not provided
    if(!g_host) {
        FILE* f = fopen("/data/local/tmp/audio_bridge.conf", "r");
        static char hbuf[256];
        if(f && fscanf(f, "%255s", hbuf) == 1) {
            g_host = hbuf;
            fclose(f);
        }
    }
    
    if(!g_host) {
        fprintf(stderr, 
            "Usage: %s --host <VPS_IP> [--port N] [--socket PATH] [--daemon]\n\n"
            "Audio Bridge v%d.%d.%d - Full telephony & SMS control\n"
            "Server commands available via TCP control channel.\n",
            argv[0], VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        return 1;
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    
    // Initialize
    log_init();
    write_pid_file();
    
    LOGI("╔══════════════════════════════════════════════════════════════╗");
    LOGI("║     Audio Bridge v%d.%d.%d - Full Telephony & SMS Control    ║", 
         VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    LOGI("╚══════════════════════════════════════════════════════════════╝");
    LOGI("Target: %s:%d", g_host, g_port);
    LOGI("Device ID: %s", get_device_id().c_str());
    
    // Setup shared memory
    if(!setup_shared_memory()) {
        LOGE("Failed to setup shared memory");
        return 1;
    }
    
    // Initialize JNI
    init_jni();
    
    // Start Unix socket server
    std::thread unix_thread(unix_socket_server_thread);
    
    // Main connection loop
    while(g_running) {
        g_connected = false;
        LOGI("Connecting to %s:%d...", g_host, g_port);
        
        int fd = tcp_connect(g_host, g_port);
        if(fd < 0) {
            LOGW("Connection failed: %s, retrying in 15s", strerror(errno));
            sleep(15);
            continue;
        }
        
        if(!handshake(fd)) {
            LOGW("Handshake failed, retrying in 15s");
            close(fd);
            sleep(15);
            continue;
        }
        
        g_connected = true;
        LOGI("Connected to server!");
        
        // Wait for Zygisk module
        auto* layout = (SharedMemoryLayout*)g_shm_ptr;
        int wait_count = 0;
        while(!layout->module_active && g_running && wait_count < 50) {
            usleep(100000);
            wait_count++;
        }
        
        if(layout->module_active) {
            LOGI("Zygisk module active - full audio interception enabled");
        } else {
            LOGW("Zygisk module not detected - audio features limited");
        }
        
        // Start worker threads
        std::thread status_thread(status_sender_thread, fd);
        std::thread speaker_thread(capture_speaker_thread, fd);
        std::thread mic_thread(receive_virtual_mic_thread, fd);
        
        status_thread.join();
        speaker_thread.join();
        mic_thread.join();
        
        close(fd);
        g_connected = false;
        LOGI("Disconnected, reconnecting in 15s");
        sleep(15);
    }
    
    unix_thread.join();
    
    // Cleanup
    if(g_shm_ptr) munmap(g_shm_ptr, SHM_SIZE);
    if(g_shm_fd >= 0) close(g_shm_fd);
    if(g_log_file) fclose(g_log_file);
    
    remove_pid_file();
    LOGI("Audio Bridge terminated");
    
    return 0;
}
```

### 2. **Java Helper Class** - `java/com/audiobridge/TelephonyHelper.java`

```java
package com.audiobridge;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Telephony;
import android.telecom.TelecomManager;
import android.telephony.PhoneStateListener;
import android.telephony.SmsManager;
import android.telephony.SmsMessage;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.telephony.PreciseCallState;
import androidx.core.app.ActivityCompat;
import android.Manifest;

import java.util.ArrayList;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

/**
 * TelephonyHelper - Provides call control and SMS functionality
 * Used by Audio Bridge native code via JNI
 */
public class TelephonyHelper {
    private static final String TAG = "AudioBridge-Telephony";
    private static TelephonyHelper sInstance;
    
    private Context mContext;
    private TelephonyManager mTelephonyManager;
    private TelecomManager mTelecomManager;
    private SmsManager mSmsManager;
    private Handler mMainHandler;
    
    private final Map<String, SMSInfo> mPendingSMS = new ConcurrentHashMap<>();
    private final Map<String, CallInfo> mActiveCalls = new ConcurrentHashMap<>();
    private String mCurrentActiveCall = null;
    
    // Native methods
    private native void nativeOnCallStateChanged(int state, String number);
    private native void nativeOnCallWaiting(String incomingNumber, String currentNumber);
    private native void nativeOnSMSSent(String messageId, int resultCode);
    private native void nativeOnSMSDelivered(String messageId);
    private native void nativeOnSMSReceived(String sender, String message, long timestamp);
    
    static {
        System.loadLibrary("audiobridge");
    }
    
    // Singleton
    public static synchronized TelephonyHelper getInstance(Context context) {
        if (sInstance == null) {
            sInstance = new TelephonyHelper(context.getApplicationContext());
        }
        return sInstance;
    }
    
    public static TelephonyHelper getInstance() {
        return sInstance;
    }
    
    private TelephonyHelper(Context context) {
        mContext = context;
        mMainHandler = new Handler(Looper.getMainLooper());
        mTelephonyManager = (TelephonyManager) context.getSystemService(Context.TELEPHONY_SERVICE);
        mTelecomManager = (TelecomManager) context.getSystemService(Context.TELECOM_SERVICE);
        mSmsManager = SmsManager.getDefault();
        
        registerCallListener();
        registerSMSReceiver();
        
        android.util.Log.i(TAG, "TelephonyHelper initialized");
    }
    
    private void registerCallListener() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            mTelephonyManager.registerTelephonyCallback(
                mContext.getMainExecutor(),
                new TelephonyCallback() {
                    @Override
                    public void onCallStateChanged(int state) {
                        handleCallStateChange(state, "");
                    }
                    
                    @Override
                    public void onPreciseCallStateChanged(PreciseCallState state) {
                        handlePreciseCallState(state);
                    }
                }
            );
        } else {
            PhoneStateListener listener = new PhoneStateListener() {
                @Override
                public void onCallStateChanged(int state, String incomingNumber) {
                    handleCallStateChange(state, incomingNumber);
                }
            };
            mTelephonyManager.listen(listener, PhoneStateListener.LISTEN_CALL_STATE);
        }
    }
    
    private void handleCallStateChange(int state, String number) {
        mMainHandler.post(() -> {
            boolean isCallWaiting = !mActiveCalls.isEmpty() && 
                                   state == TelephonyManager.CALL_STATE_RINGING;
            
            if (isCallWaiting) {
                String currentNumber = mCurrentActiveCall != null ? 
                    mActiveCalls.get(mCurrentActiveCall).number : "";
                nativeOnCallWaiting(number, currentNumber);
            }
            
            updateCallTracking(state, number);
            nativeOnCallStateChanged(state, number);
        });
    }
    
    private void handlePreciseCallState(PreciseCallState state) {
        // Forward detailed call state information
        mMainHandler.post(() -> {
            android.util.Log.d(TAG, "Precise call state: fg=" + state.getForegroundCallState() +
                               ", bg=" + state.getBackgroundCallState());
        });
    }
    
    private void updateCallTracking(int state, String number) {
        if (state == TelephonyManager.CALL_STATE_IDLE) {
            mActiveCalls.clear();
            mCurrentActiveCall = null;
        } else if (state == TelephonyManager.CALL_STATE_RINGING) {
            String callId = "call_" + System.currentTimeMillis();
            mActiveCalls.put(callId, new CallInfo(number, state, true));
        } else if (state == TelephonyManager.CALL_STATE_OFFHOOK) {
            if (mCurrentActiveCall == null) {
                String callId = "call_" + System.currentTimeMillis();
                mCurrentActiveCall = callId;
                mActiveCalls.put(callId, new CallInfo(number, state, false));
            }
        }
    }
    
    private void registerSMSReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(Telephony.Sms.Intents.SMS_RECEIVED_ACTION);
        mContext.registerReceiver(new SMSBroadcastReceiver(), filter);
    }
    
    // Public API - Call Control
    
    public void placeCall(String number) {
        if (ActivityCompat.checkSelfPermission(mContext, Manifest.permission.CALL_PHONE) 
                != PackageManager.PERMISSION_GRANTED) {
            android.util.Log.w(TAG, "CALL_PHONE permission not granted");
            return;
        }
        
        Intent intent = new Intent(Intent.ACTION_CALL);
        intent.setData(Uri.parse("tel:" + number));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        mContext.startActivity(intent);
        
        android.util.Log.i(TAG, "Placing call to: " + number);
    }
    
    public void endCall() {
        if (mTelecomManager != null) {
            if (ActivityCompat.checkSelfPermission(mContext, Manifest.permission.ANSWER_PHONE_CALLS) 
                    == PackageManager.PERMISSION_GRANTED) {
                mTelecomManager.endCall();
                android.util.Log.i(TAG, "Call ended");
            }
        }
    }
    
    public void answerCall() {
        if (mTelecomManager != null) {
            if (ActivityCompat.checkSelfPermission(mContext, Manifest.permission.ANSWER_PHONE_CALLS) 
                    == PackageManager.PERMISSION_GRANTED) {
                mTelecomManager.acceptRingingCall();
                android.util.Log.i(TAG, "Call answered");
            }
        }
    }
    
    // Public API - SMS
    
    public String sendSMS(String phoneNumber, String message) {
        if (ActivityCompat.checkSelfPermission(mContext, Manifest.permission.SEND_SMS) 
                != PackageManager.PERMISSION_GRANTED) {
            android.util.Log.w(TAG, "SEND_SMS permission not granted");
            return null;
        }
        
        String messageId = UUID.randomUUID().toString();
        
        SMSInfo info = new SMSInfo();
        info.id = messageId;
        info.number = phoneNumber;
        info.message = message;
        info.timestamp = System.currentTimeMillis();
        
        ArrayList<String> parts = mSmsManager.divideMessage(message);
        info.parts = parts;
        info.totalParts = parts.size();
        
        mPendingSMS.put(messageId, info);
        
        ArrayList<PendingIntent> sentIntents = new ArrayList<>();
        ArrayList<PendingIntent> deliveryIntents = new ArrayList<>();
        
        for (int i = 0; i < parts.size(); i++) {
            Intent sentIntent = new Intent("SMS_SENT_" + messageId);
            sentIntent.putExtra("message_id", messageId);
            sentIntent.putExtra("part", i);
            PendingIntent sentPI = PendingIntent.getBroadcast(
                mContext, i, sentIntent, 
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            sentIntents.add(sentPI);
            
            Intent deliveryIntent = new Intent("SMS_DELIVERED_" + messageId);
            deliveryIntent.putExtra("message_id", messageId);
            PendingIntent deliveryPI = PendingIntent.getBroadcast(
                mContext, i + 1000, deliveryIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            deliveryIntents.add(deliveryPI);
        }
        
        try {
            mSmsManager.sendMultipartTextMessage(phoneNumber, null, parts, 
                                                  sentIntents, deliveryIntents);
            registerSMSReceivers(messageId);
            android.util.Log.i(TAG, "SMS sent to " + phoneNumber + " (ID: " + messageId + ")");
        } catch (Exception e) {
            android.util.Log.e(TAG, "Failed to send SMS", e);
            nativeOnSMSSent(messageId, 1);
            mPendingSMS.remove(messageId);
            return null;
        }
        
        return messageId;
    }
    
    private void registerSMSReceivers(String messageId) {
        IntentFilter sentFilter = new IntentFilter("SMS_SENT_" + messageId);
        IntentFilter deliveredFilter = new IntentFilter("SMS_DELIVERED_" + messageId);
        
        BroadcastReceiver sentReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String id = intent.getStringExtra("message_id");
                SMSInfo info = mPendingSMS.get(id);
                
                if (info != null) {
                    int resultCode = getResultCode();
                    
                    if (resultCode == Activity.RESULT_OK) {
                        info.sentParts++;
                        if (info.sentParts == info.totalParts) {
                            nativeOnSMSSent(id, -1);
                        }
                    } else {
                        nativeOnSMSSent(id, resultCode);
                        mPendingSMS.remove(id);
                    }
                }
                
                try {
                    mContext.unregisterReceiver(this);
                } catch (IllegalArgumentException e) {}
            }
        };
        
        BroadcastReceiver deliveredReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String id = intent.getStringExtra("message_id");
                SMSInfo info = mPendingSMS.get(id);
                
                if (info != null) {
                    if (getResultCode() == Activity.RESULT_OK) {
                        nativeOnSMSDelivered(id);
                        mPendingSMS.remove(id);
                    }
                }
                
                try {
                    mContext.unregisterReceiver(this);
                } catch (IllegalArgumentException e) {}
            }
        };
        
        mContext.registerReceiver(sentReceiver, sentFilter, 
                                  Context.RECEIVER_EXPORTED);
        mContext.registerReceiver(deliveredReceiver, deliveredFilter,
                                  Context.RECEIVER_EXPORTED);
    }
    
    // Inner classes
    
    private static class CallInfo {
        String number;
        int state;
        long startTime;
        boolean isIncoming;
        
        CallInfo(String num, int st, boolean incoming) {
            number = num;
            state = st;
            startTime = System.currentTimeMillis();
            isIncoming = incoming;
        }
    }
    
    private static class SMSInfo {
        String id;
        String number;
        String message;
        ArrayList<String> parts;
        int totalParts;
        int sentParts;
        long timestamp;
    }
    
    private class SMSBroadcastReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Telephony.Sms.Intents.SMS_RECEIVED_ACTION.equals(intent.getAction())) {
                Bundle bundle = intent.getExtras();
                if (bundle != null) {
                    Object[] pdus = (Object[]) bundle.get("pdus");
                    if (pdus != null) {
                        for (Object pdu : pdus) {
                            SmsMessage sms = SmsMessage.createFromPdu((byte[]) pdu);
                            String sender = sms.getDisplayOriginatingAddress();
                            String message = sms.getDisplayMessageBody();
                            long timestamp = sms.getTimestampMillis();
                            
                            nativeOnSMSReceived(sender, message, timestamp);
                        }
                    }
                }
            }
        }
    }
}
```

### 3. **Zygisk Module** - `zygisk/src/zygisk_module.cpp`

```cpp
#include <jni.h>
#include <string>
#include <atomic>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <android/log.h>
#include <chrono>
#include "zygisk.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AudioBridge-Zygisk", __VA_ARGS__)
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
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return;
        
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return;
        }
        
        send(fd, "GET_SHM_FD", 10, 0);
        
        struct msghdr msg = {};
        char buf[CMSG_SPACE(sizeof(int))];
        memset(buf, 0, sizeof(buf));
        
        struct iovec io = { .iov_base = (void*)"OK", .iov_len = 2 };
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);
        
        if (recvmsg(fd, &msg, 0) < 0) {
            close(fd);
            return;
        }
        
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
        
        close(fd);
        
        if (g_active) {
            // Hook AudioTrack::write
            api->pltHookRegister("libaudioclient.so", 
                "_ZN7android10AudioTrack5writeEPKvj", 
                (void*)hooked_audio_track_write, 
                (void**)&original_audio_track_write);
            
            // Hook AudioRecord::read    
            api->pltHookRegister("libaudioclient.so",
                "_ZN7android10AudioRecord4readEPvj",
                (void*)hooked_audio_record_read,
                (void**)&original_audio_record_read);
                
            LOGI("Audio hooks installed");
        }
    }
    
private:
    zygisk::Api* api = nullptr;
    JNIEnv* env = nullptr;
};

REGISTER_ZYGISK_MODULE(AudioBridgeModule)
```

---

## 📋 Build Instructions

### `build.sh`

```bash
#!/bin/bash
# Audio Bridge Build Script
set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║           Audio Bridge - Full Build Script v3.0              ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"

# Configuration
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/25.2.9519653}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
LIBS_DIR="$PROJECT_DIR/libs"
API_LEVEL=28

# Check NDK
if [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo -e "${RED}Error: Android NDK not found at $ANDROID_NDK_HOME${NC}"
    echo "Set ANDROID_NDK_HOME environment variable or install NDK r25+"
    exit 1
fi

echo -e "${YELLOW}Using NDK: $ANDROID_NDK_HOME${NC}"

# Create directories
mkdir -p "$BUILD_DIR"
mkdir -p "$LIBS_DIR/arm64-v8a"
mkdir -p "$LIBS_DIR/armeabi-v7a"

# Build Opus for arm64
build_opus() {
    local ARCH=$1
    local ABI=$2
    local TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
    
    echo -e "${YELLOW}Building Opus for $ARCH...${NC}"
    
    cd "$BUILD_DIR"
    if [ ! -d "opus" ]; then
        git clone --depth 1 https://github.com/xiph/opus.git
    fi
    cd opus
    
    # Clean previous builds
    make clean 2>/dev/null || true
    
    export CC="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang"
    export CXX="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang++"
    export AR="$TOOLCHAIN/bin/llvm-ar"
    export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
    
    ./autogen.sh 2>/dev/null || true
    ./configure \
        --host="${ARCH}-linux-android" \
        --prefix="$BUILD_DIR/opus-install-$ABI" \
        --disable-shared \
        --enable-static \
        --disable-doc \
        --disable-extra-programs \
        CFLAGS="-O3 -fPIC"
    
    make -j$(nproc)
    make install
    
    cp "$BUILD_DIR/opus-install-$ABI/lib/libopus.a" "$LIBS_DIR/$ABI/"
    echo -e "${GREEN}Opus built for $ARCH${NC}"
}

# Build TinyALSA
build_tinyalsa() {
    local ARCH=$1
    local ABI=$2
    local TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
    
    echo -e "${YELLOW}Building TinyALSA for $ARCH...${NC}"
    
    cd "$BUILD_DIR"
    if [ ! -d "tinyalsa" ]; then
        git clone --depth 1 https://github.com/tinyalsa/tinyalsa.git
    fi
    cd tinyalsa
    
    export CC="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang"
    export AR="$TOOLCHAIN/bin/llvm-ar"
    
    make clean 2>/dev/null || true
    make \
        CROSS_COMPILE="$TOOLCHAIN/bin/${ARCH}-linux-android-" \
        CFLAGS="-O3 -fPIC -DANDROID"
    
    mkdir -p "$LIBS_DIR/$ABI/include/tinyalsa"
    cp libtinyalsa.a "$LIBS_DIR/$ABI/"
    cp include/tinyalsa/*.h "$LIBS_DIR/$ABI/include/tinyalsa/"
    
    echo -e "${GREEN}TinyALSA built for $ARCH${NC}"
}

# Build main native binary
build_native() {
    local ARCH=$1
    local ABI=$2
    local TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
    
    echo -e "${YELLOW}Building Audio Bridge native for $ARCH...${NC}"
    
    export CC="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang"
    export CXX="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang++"
    
    cd "$PROJECT_DIR/jni"
    
    $CXX \
        -std=c++17 \
        -O3 \
        -fPIE \
        -DANDROID \
        -D__ANDROID_API__=$API_LEVEL \
        -I"$LIBS_DIR/$ABI/include" \
        -I"$LIBS_DIR/$ABI/include/tinyalsa" \
        -I"$PROJECT_DIR/jni" \
        audio_bridge.cpp \
        -o "$BUILD_DIR/audio-bridge-$ABI" \
        -L"$LIBS_DIR/$ABI" \
        -lopus \
        -ltinyalsa \
        -pthread \
        -pie \
        -Wl,--gc-sections \
        -Wl,--strip-all \
        -llog
    
    echo -e "${GREEN}Native binary built: $BUILD_DIR/audio-bridge-$ABI${NC}"
}

# Build Java helper APK
build_apk() {
    echo -e "${YELLOW}Building TelephonyHelper APK...${NC}"
    
    cd "$PROJECT_DIR"
    
    # Create Android project structure
    mkdir -p app/src/main/java/com/audiobridge
    mkdir -p app/src/main/res/values
    mkdir -p app/libs
    
    # Copy Java source
    cp java/com/audiobridge/TelephonyHelper.java app/src/main/java/com/audiobridge/
    
    # Create AndroidManifest.xml
    cat > app/src/main/AndroidManifest.xml << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.audiobridge">

    <uses-permission android:name="android.permission.CALL_PHONE" />
    <uses-permission android:name="android.permission.ANSWER_PHONE_CALLS" />
    <uses-permission android:name="android.permission.READ_PHONE_STATE" />
    <uses-permission android:name="android.permission.SEND_SMS" />
    <uses-permission android:name="android.permission.RECEIVE_SMS" />
    <uses-permission android:name="android.permission.READ_SMS" />
    <uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />

    <application
        android:allowBackup="true"
        android:supportsRtl="true">
        
        <service
            android:name=".AudioBridgeService"
            android:enabled="true"
            android:exported="false"
            android:foregroundServiceType="phoneCall|microphone" />
        
        <receiver
            android:name=".BootReceiver"
            android:enabled="true"
            android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.BOOT_COMPLETED" />
            </intent-filter>
        </receiver>
    </application>
</manifest>
EOF

    # Create strings.xml
    cat > app/src/main/res/values/strings.xml << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">Audio Bridge</string>
</resources>
EOF

    # Copy native library
    mkdir -p app/src/main/jniLibs/arm64-v8a
    mkdir -p app/src/main/jniLibs/armeabi-v7a
    cp "$BUILD_DIR/libaudiobridge-arm64-v8a.so" app/src/main/jniLibs/arm64-v8a/libaudiobridge.so 2>/dev/null || true
    
    # Build APK using gradle
    cat > app/build.gradle << 'EOF'
plugins {
    id 'com.android.application'
}

android {
    namespace 'com.audiobridge'
    compileSdk 34
    
    defaultConfig {
        applicationId "com.audiobridge"
        minSdk 28
        targetSdk 34
        versionCode 1
        versionName "1.0"
    }
    
    buildTypes {
        release {
            minifyEnabled false
        }
    }
    
    compileOptions {
        sourceCompatibility JavaVersion.VERSION_1_8
        targetCompatibility JavaVersion.VERSION_1_8
    }
}
EOF

    echo -e "${GREEN}APK source prepared${NC}"
    echo -e "${YELLOW}Build APK using Android Studio or ./gradlew assembleRelease${NC}"
}

# Build Zygisk module
build_zygisk() {
    echo -e "${YELLOW}Building Zygisk module...${NC}"
    
    cd "$PROJECT_DIR/zygisk"
    
    local TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
    export CC="$TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang"
    export CXX="$TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang++"
    
    # Download Zygisk headers if needed
    if [ ! -f "zygisk.hpp" ]; then
        curl -L -o zygisk.hpp https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp
    fi
    
    mkdir -p "$PROJECT_DIR/zygisk/module"
    mkdir -p "$PROJECT_DIR/zygisk/module/zygisk"
    
    # Compile Zygisk module
    $CXX \
        -std=c++17 \
        -O3 \
        -fPIC \
        -shared \
        -DANDROID \
        -D__ANDROID_API__=$API_LEVEL \
        -I"$PROJECT_DIR/zygisk" \
        src/zygisk_module.cpp \
        -o "$PROJECT_DIR/zygisk/module/zygisk/arm64-v8a.so" \
        -llog
    
    # Create module.prop
    cat > "$PROJECT_DIR/zygisk/module/module.prop" << EOF
id=audio_bridge
name=Audio Bridge
version=v3.0
versionCode=300
author=AudioBridge
description=Virtual microphone and speaker capture for remote audio
EOF

    # Create post-fs-data.sh
    cat > "$PROJECT_DIR/zygisk/module/post-fs-data.sh" << 'EOF'
#!/system/bin/sh
# Wait for audio bridge daemon
sleep 5
if [ ! -S /data/local/tmp/audio_bridge.sock ]; then
    /data/local/tmp/audio-bridge --daemon &
fi
EOF
    chmod +x "$PROJECT_DIR/zygisk/module/post-fs-data.sh"
    
    echo -e "${GREEN}Zygisk module built${NC}"
}

# Build all
main() {
    echo -e "${YELLOW}Starting full build...${NC}"
    
    # Build dependencies for arm64
    build_opus "aarch64" "arm64-v8a"
    build_tinyalsa "aarch64" "arm64-v8a"
    
    # Build native binary
    build_native "aarch64" "arm64-v8a"
    
    # Build JNI library
    build_jni_lib "aarch64" "arm64-v8a"
    
    # Build Zygisk module
    build_zygisk
    
    # Prepare APK sources
    build_apk
    
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                    Build Complete!                           ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "Output files:"
    echo -e "  Native binary: ${YELLOW}$BUILD_DIR/audio-bridge-arm64-v8a${NC}"
    echo -e "  Zygisk module: ${YELLOW}$PROJECT_DIR/zygisk/module/${NC}"
    echo -e "  APK source:    ${YELLOW}$PROJECT_DIR/app/${NC}"
    echo ""
    echo -e "Installation:"
    echo -e "  1. Push binary: ${YELLOW}adb push $BUILD_DIR/audio-bridge-arm64-v8a /data/local/tmp/audio-bridge${NC}"
    echo -e "  2. Push Zygisk: ${YELLOW}adb push $PROJECT_DIR/zygisk/module /data/adb/modules/audio_bridge${NC}"
    echo -e "  3. Install APK: ${YELLOW}cd app && ./gradlew installRelease${NC}"
}

# Build JNI library
build_jni_lib() {
    local ARCH=$1
    local ABI=$2
    local TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
    
    echo -e "${YELLOW}Building JNI library for $ARCH...${NC}"
    
    export CC="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang"
    export CXX="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang++"
    
    cd "$PROJECT_DIR/jni"
    
    $CXX \
        -std=c++17 \
        -O3 \
        -fPIC \
        -shared \
        -DANDROID \
        -D__ANDROID_API__=$API_LEVEL \
        jni_bridge.cpp \
        -o "$BUILD_DIR/libaudiobridge-$ABI.so" \
        -llog
    
    echo -e "${GREEN}JNI library built: $BUILD_DIR/libaudiobridge-$ABI.so${NC}"
}

main "$@"
```

---

## 📡 Server Protocol Documentation

### `server/protocol.md`

```markdown
# Audio Bridge Server Protocol v3.0

## Overview

The Audio Bridge uses a custom multiplexed TCP protocol with the following frame structure:

```
[1 byte Type][4 bytes Length][Payload Data]
```

### Frame Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| T_SPEAKER | 0x01 | Phone → Server | Opus-encoded speaker audio |
| T_VIRTUAL_MIC | 0x02 | Server → Phone | Opus-encoded virtual mic audio |
| T_CONTROL | 0x03 | Server → Phone | Control commands (JSON) |
| T_CALL_STATUS | 0x04 | Phone → Server | Call status updates (JSON) |
| T_SMS | 0x05 | Both | SMS control and status (JSON) |
| T_PING | 0x06 | Server → Phone | Keepalive ping |
| T_PONG | 0x07 | Phone → Server | Keepalive response |

## Connection Flow

1. **TCP Connect** - Connect to phone on port 59100
2. **Registration** - Phone sends JSON registration
3. **Ready** - Server responds with `{"status":"ok"}\n`

### Registration (Phone → Server)
```json
{
  "type": "register",
  "name": "Pixel 8 Pro",
  "brand": "Google",
  "android": "14",
  "id": "abc123def456",
  "mode": "full_control",
  "version": "3.0.0",
  "features": ["audio", "call_control", "sms"]
}
```

## Server Commands (T_CONTROL)

### Dial a Number
```json
{
  "command": "dial",
  "number": "+1234567890"
}
```

### End Current Call
```json
{
  "command": "hangup"
}
```

### Answer Incoming Call
```json
{
  "command": "answer"
}
```

### Hold Active and Answer Waiting
```json
{
  "command": "hold_and_answer"
}
```

### Hangup Active and Answer Waiting
```json
{
  "command": "hangup_and_answer"
}
```

### Send SMS
```json
{
  "command": "send_sms",
  "number": "+1234567890",
  "message": "Hello from remote!"
}
```

### Get SMS Status
```json
{
  "command": "get_sms_status",
  "message_id": "sms_12345"
}
```

### List Recent SMS
```json
{
  "command": "list_sms"
}
```

### Ping (Keepalive)
```json
{
  "command": "ping"
}
```

## Phone Events (T_CALL_STATUS)

### Call State Change
```json
{
  "type": "call_status",
  "state": 2,
  "state_name": "OFFHOOK",
  "number": "+1234567890",
  "timestamp": 1713524400
}
```

**State Values:**
- 0 = IDLE (no call)
- 1 = RINGING (incoming)
- 2 = OFFHOOK (active call)
- 3 = DIALING (outgoing)
- 4 = HOLDING

### Call Waiting
```json
{
  "type": "call_waiting",
  "incoming_number": "+1987654321",
  "active_number": "+1234567890",
  "timestamp": 1713524400,
  "available_actions": ["hold_and_answer", "hangup_and_answer", "ignore"]
}
```

### SMS Sent Confirmation
```json
{
  "type": "sms_status",
  "message_id": "sms_1713524400_12345",
  "result": "sent",
  "result_code": -1,
  "timestamp": 1713524400
}
```

### SMS Delivered
```json
{
  "type": "sms_status",
  "message_id": "sms_1713524400_12345",
  "status": "delivered",
  "timestamp": 1713524405
}
```

### SMS Received
```json
{
  "type": "sms_received",
  "sender": "+1987654321",
  "message": "Reply message",
  "timestamp": 1713524500
}
```

## Audio Format

- **Codec**: Opus
- **Sample Rate**: 48kHz
- **Channels**: 1 (Mono)
- **Frame Size**: 20ms (960 samples)
- **Bitrate**: 64kbps (speaker), 32kbps (mic)
- **FEC**: Enabled
- **PLC**: Enabled
```

---

## 🐍 Server Example (Python)

### `server/server_example.py`

```python
#!/usr/bin/env python3
"""
Audio Bridge Server Example
Connects to Android phone and provides remote control interface
"""

import socket
import struct
import json
import threading
import time
import opuslib
import pyaudio
import queue
import cmd
import sys

class AudioBridgeClient:
    """Client connection to a single Android device"""
    
    # Frame types
    T_SPEAKER = 0x01
    T_VIRTUAL_MIC = 0x02
    T_CONTROL = 0x03
    T_CALL_STATUS = 0x04
    T_SMS = 0x05
    T_PING = 0x06
    T_PONG = 0x07
    
    def __init__(self, host, port=59100):
        self.host = host
        self.port = port
        self.sock = None
        self.running = False
        self.device_info = {}
        self.call_state = 0
        self.current_number = ""
        
        # Audio
        self.SAMPLE_RATE = 48000
        self.CHANNELS = 1
        self.FRAME_SAMPLES = 960
        
        # Opus codecs
        self.opus_decoder = opuslib.Decoder(self.SAMPLE_RATE, self.CHANNELS)
        self.opus_encoder = opuslib.Encoder(self.SAMPLE_RATE, self.CHANNELS, 'voip')
        self.opus_encoder.bitrate = 32000
        
        # Audio queues
        self.speaker_queue = queue.Queue(maxsize=100)
        self.mic_queue = queue.Queue(maxsize=100)
        
        # PyAudio
        self.pa = pyaudio.PyAudio()
        self.speaker_stream = None
        self.mic_stream = None
        
        # Event handlers
        self.on_call_status = None
        self.on_sms_status = None
        self.on_sms_received = None
    
    def connect(self):
        """Connect to the device"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        
        try:
            self.sock.connect((self.host, self.port))
            print(f"[+] Connected to {self.host}:{self.port}")
            
            # Wait for registration
            reg_data = self._recv_line()
            reg = json.loads(reg_data)
            self.device_info = reg
            print(f"[+] Device: {reg.get('name')} ({reg.get('id')})")
            
            # Send OK
            self.sock.send(b'{"status":"ok"}\n')
            
            self.running = True
            return True
            
        except Exception as e:
            print(f"[-] Connection failed: {e}")
            return False
    
    def _recv_line(self):
        """Receive a line (until newline)"""
        data = b''
        while True:
            c = self.sock.recv(1)
            if not c or c == b'\n':
                break
            data += c
        return data.decode('utf-8')
    
    def _recv_frame(self):
        """Receive a framed message"""
        hdr = self._recv_exact(5)
        if not hdr:
            return None, None
        
        frame_type = hdr[0]
        length = struct.unpack('>I', hdr[1:5])[0]
        
        data = self._recv_exact(length)
        return frame_type, data
    
    def _recv_exact(self, length):
        """Receive exact number of bytes"""
        data = b''
        while len(data) < length:
            chunk = self.sock.recv(length - len(data))
            if not chunk:
                return None
            data += chunk
        return data
    
    def send_frame(self, frame_type, data):
        """Send a framed message"""
        hdr = struct.pack('>BI', frame_type, len(data))
        self.sock.send(hdr + data)
    
    def send_control(self, command_data):
        """Send a control command"""
        if isinstance(command_data, dict):
            command_data = json.dumps(command_data)
        self.send_frame(self.T_CONTROL, command_data.encode())
    
    def send_audio_frame(self, pcm_data):
        """Send microphone audio to phone"""
        opus_data = self.opus_encoder.encode(pcm_data, self.FRAME_SAMPLES)
        self.send_frame(self.T_VIRTUAL_MIC, opus_data)
    
    # Commands
    
    def dial(self, number):
        """Dial a number"""
        self.send_control({"command": "dial", "number": number})
        print(f"[+] Dialing {number}")
    
    def hangup(self):
        """End current call"""
        self.send_control({"command": "hangup"})
        print("[+] Hanging up")
    
    def answer(self):
        """Answer incoming call"""
        self.send_control({"command": "answer"})
        print("[+] Answering call")
    
    def send_sms(self, number, message):
        """Send SMS"""
        self.send_control({
            "command": "send_sms",
            "number": number,
            "message": message
        })
        print(f"[+] Sending SMS to {number}")
    
    def ping(self):
        """Send keepalive ping"""
        self.send_control({"command": "ping"})
    
    # Audio callbacks
    
    def _speaker_callback(self, in_data, frame_count, time_info, status):
        """Called when speaker data is available (from phone)"""
        if self.speaker_stream:
            self.speaker_stream.write(in_data)
        return (None, pyaudio.paContinue)
    
    def _mic_callback(self, in_data, frame_count, time_info, status):
        """Called when microphone data is available (to send to phone)"""
        try:
            self.mic_queue.put_nowait(in_data)
        except queue.Full:
            pass
        return (None, pyaudio.paContinue)
    
    def start_audio(self):
        """Start audio streams"""
        # Speaker output (from phone to speakers)
        self.speaker_stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.CHANNELS,
            rate=self.SAMPLE_RATE,
            output=True,
            frames_per_buffer=self.FRAME_SAMPLES
        )
        
        # Microphone input (to send to phone)
        self.mic_stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.CHANNELS,
            rate=self.SAMPLE_RATE,
            input=True,
            frames_per_buffer=self.FRAME_SAMPLES,
            stream_callback=self._mic_callback
        )
        
        self.mic_stream.start_stream()
        print("[+] Audio streams started")
    
    def stop_audio(self):
        """Stop audio streams"""
        if self.speaker_stream:
            self.speaker_stream.stop_stream()
            self.speaker_stream.close()
        if self.mic_stream:
            self.mic_stream.stop_stream()
            self.mic_stream.close()
        print("[+] Audio streams stopped")
    
    def run(self):
        """Main receive loop"""
        self.start_audio()
        
        last_ping = time.time()
        
        while self.running:
            try:
                # Check for ping
                if time.time() - last_ping > 30:
                    self.ping()
                    last_ping = time.time()
                
                # Receive frame
                frame_type, data = self._recv_frame()
                
                if frame_type is None:
                    break
                
                if frame_type == self.T_SPEAKER:
                    # Decode and queue speaker audio
                    pcm = self.opus_decoder.decode(data, self.FRAME_SAMPLES)
                    if self.speaker_stream:
                        self.speaker_stream.write(pcm)
                
                elif frame_type == self.T_CALL_STATUS:
                    # Parse call status
                    status = json.loads(data.decode())
                    self.call_state = status.get('state', 0)
                    self.current_number = status.get('number', '')
                    
                    print(f"[Call] {status.get('state_name')} - {self.current_number}")
                    
                    if self.on_call_status:
                        self.on_call_status(status)
                
                elif frame_type == self.T_SMS:
                    # Parse SMS status
                    status = json.loads(data.decode())
                    
                    if status.get('type') == 'sms_status':
                        print(f"[SMS] {status.get('message_id')}: {status.get('result', status.get('status'))}")
                        if self.on_sms_status:
                            self.on_sms_status(status)
                    
                    elif status.get('type') == 'sms_received':
                        print(f"[SMS] From: {status.get('sender')}: {status.get('message')}")
                        if self.on_sms_received:
                            self.on_sms_received(status)
                
                elif frame_type == self.T_PONG:
                    pass  # Keepalive response
                
                # Send microphone audio if available
                try:
                    mic_data = self.mic_queue.get_nowait()
                    self.send_audio_frame(mic_data)
                except queue.Empty:
                    pass
                
            except socket.timeout:
                continue
            except Exception as e:
                print(f"[-] Error: {e}")
                break
        
        self.stop_audio()
        self.disconnect()
    
    def disconnect(self):
        """Disconnect from device"""
        self.running = False
        if self.sock:
            self.sock.close()
        print("[+] Disconnected")


class AudioBridgeConsole(cmd.Cmd):
    """Interactive console for Audio Bridge control"""
    
    intro = """
╔══════════════════════════════════════════════════════════════╗
║           Audio Bridge Server Console v3.0                   ║
║      Type 'help' for available commands                     ║
╚══════════════════════════════════════════════════════════════╝
"""
    prompt = 'audio-bridge> '
    
    def __init__(self, client):
        super().__init__()
        self.client = client
    
    def do_dial(self, arg):
        """dial <number> - Dial a phone number"""
        if arg:
            self.client.dial(arg)
        else:
            print("Usage: dial <number>")
    
    def do_hangup(self, arg):
        """hangup - End current call"""
        self.client.hangup()
    
    def do_answer(self, arg):
        """answer - Answer incoming call"""
        self.client.answer()
    
    def do_sms(self, arg):
        """sms <number> <message> - Send SMS"""
        parts = arg.split(' ', 1)
        if len(parts) == 2:
            self.client.send_sms(parts[0], parts[1])
        else:
            print("Usage: sms <number> <message>")
    
    def do_status(self, arg):
        """status - Show current call status"""
        state_names = ['IDLE', 'RINGING', 'OFFHOOK', 'DIALING', 'HOLDING']
        state = self.client.call_state
        print(f"Call State: {state_names[state] if state < 5 else 'UNKNOWN'}")
        if self.client.current_number:
            print(f"Number: {self.client.current_number}")
    
    def do_info(self, arg):
        """info - Show device information"""
        print(f"Device: {self.client.device_info.get('name', 'Unknown')}")
        print(f"Brand: {self.client.device_info.get('brand', 'Unknown')}")
        print(f"Android: {self.client.device_info.get('android', 'Unknown')}")
        print(f"ID: {self.client.device_info.get('id', 'Unknown')}")
    
    def do_quit(self, arg):
        """quit - Disconnect and exit"""
        self.client.running = False
        return True
    
    def do_exit(self, arg):
        """exit - Disconnect and exit"""
        return self.do_quit(arg)


def main():
    if len(sys.argv) < 2:
        print("Usage: python server_example.py <phone_ip> [port]")
        sys.exit(1)
    
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 59100
    
    client = AudioBridgeClient(host, port)
    
    if not client.connect():
        sys.exit(1)
    
    # Start receiver thread
    receiver_thread = threading.Thread(target=client.run)
    receiver_thread.daemon = True
    receiver_thread.start()
    
    # Start interactive console
    console = AudioBridgeConsole(client)
    
    try:
        console.cmdloop()
    except KeyboardInterrupt:
        print("\n[!] Interrupted")
    finally:
        client.running = False
        client.disconnect()


if __name__ == '__main__':
    main()
```

---

## 📦 Installation Script

### `scripts/install.sh`

```bash
#!/system/bin/sh
# Audio Bridge Installation Script

echo "Audio Bridge v3.0 Installation"
echo "================================"

# Check root
if [ "$(id -u)" != "0" ]; then
    echo "Error: Root required"
    exit 1
fi

# Create directories
mkdir -p /data/local/tmp
mkdir -p /data/adb/modules/audio_bridge

# Copy binary
cp /sdcard/audio-bridge /data/local/tmp/
chmod 755 /data/local/tmp/audio-bridge

# Copy Zygisk module
cp -r /sdcard/zygisk_module/* /data/adb/modules/audio_bridge/
chmod -R 755 /data/adb/modules/audio_bridge

# Create config
echo "YOUR_SERVER_IP" > /data/local/tmp/audio_bridge.conf
chmod 600 /data/local/tmp/audio_bridge.conf

# Start service
/data/local/tmp/audio-bridge --daemon &

echo "Installation complete!"
echo "Edit /data/local/tmp/audio_bridge.conf with your server IP"
echo "Reboot to activate Zygisk module"
```

---

This complete system provides:

1. **Full source code** for native binary, Java helper, and Zygisk module
2. **Build scripts** with proper dependency management
3. **Project structure** visualization
4. **Protocol documentation** for server integration
5. **Python server example** with interactive console
6. **Installation scripts** for deployment

The system enables complete remote control of Android telephony functions including calling, call waiting handling, and SMS messaging.