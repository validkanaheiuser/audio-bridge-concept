/**
 * Audio Bridge - Complete System with Telephony & SMS Control
 * Version: 3.0
 * License: MIT
 */

#include <limits.h>
#include <climits>
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
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/debug.h>
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
#include <sys/syscall.h>

#ifndef memfd_create
static inline int memfd_create(const char *name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif

// ──────────────────────────────────────────────────────────────────────────
// Configuration Constants
// ──────────────────────────────────────────────────────────────────────────

#define VERSION_MAJOR 3
#define VERSION_MINOR 0
#define VERSION_PATCH 0

static const char* g_host         = nullptr;
static int         g_port         = 59100;
static const char* g_token        = "default_secure_token_123";
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
    SimpleJson(Type t) : type(t) {}
    SimpleJson(const std::string& str) : type(STRING), string_value(str) {}
    SimpleJson(const char* str) : type(STRING), string_value(str) {}
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

static std::mutex              g_sms_mutex;
static std::map<std::string, SimpleJson> g_sms_tracking;

static std::mutex              g_log_mutex;
static FILE*                   g_log_file = nullptr;

// TLS State
static mbedtls_net_context      g_net;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_ssl_context      g_ssl;
static mbedtls_ssl_config       g_conf;
static std::mutex               g_tls_write_mutex;

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
// Network Utilities (TLS via mbedtls)
// ──────────────────────────────────────────────────────────────────────────

static bool send_all(mbedtls_ssl_context* ssl, const void* data, size_t len) {
    std::lock_guard<std::mutex> lk(g_tls_write_mutex);
    const auto* p = (const uint8_t*)data;
    while(len > 0) {
        int n = mbedtls_ssl_write(ssl, p, len);
        if(n <= 0) {
            if(n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return false;
            }
            continue;
        }
        p += n;
        len -= n;
    }
    return true;
}

static bool recv_all(mbedtls_ssl_context* ssl, void* data, size_t len) {
    auto* p = (uint8_t*)data;
    while(len > 0) {
        int n = mbedtls_ssl_read(ssl, p, len);
        if(n <= 0) {
            if(n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return false;
            }
            continue;
        }
        p += n;
        len -= n;
    }
    return true;
}

static bool send_frame(mbedtls_ssl_context* ssl, uint8_t type, const void* data, uint32_t len) {
    uint8_t hdr[5];
    hdr[0] = type;
    hdr[1] = (len >> 24) & 0xFF;
    hdr[2] = (len >> 16) & 0xFF;
    hdr[3] = (len >>  8) & 0xFF;
    hdr[4] = (len >>  0) & 0xFF;
    return send_all(ssl, hdr, 5) && send_all(ssl, data, len);
}

static bool send_json(mbedtls_ssl_context* ssl, uint8_t type, const SimpleJson& json) {
    std::string str = json.toString();
    return send_frame(ssl, type, str.c_str(), str.length());
}

static void tls_cleanup() {
    mbedtls_net_free(&g_net);
    mbedtls_ssl_free(&g_ssl);
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_ctr_drbg_free(&g_ctr_drbg);
    mbedtls_entropy_free(&g_entropy);
}

static bool tls_connect(const char* host, int port) {
    tls_cleanup();
    
    mbedtls_net_init(&g_net);
    mbedtls_ssl_init(&g_ssl);
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_entropy_init(&g_entropy);
    
    const char* pers = "audio_bridge_client";
    if(mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy, 
                             (const unsigned char*)pers, strlen(pers)) != 0) {
        LOGE("mbedtls_ctr_drbg_seed failed");
        return false;
    }
    
    char port_str[10];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    if(mbedtls_net_connect(&g_net, host, port_str, MBEDTLS_NET_PROTO_TCP) != 0) {
        return false;
    }
    
    if(mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, 
                                   MBEDTLS_SSL_TRANSPORT_STREAM, 
                                   MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE); // Disable cert verify for self-signed
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);
    
    mbedtls_ssl_setup(&g_ssl, &g_conf);
    mbedtls_ssl_set_hostname(&g_ssl, host);
    mbedtls_ssl_set_bio(&g_ssl, &g_net, mbedtls_net_send, mbedtls_net_recv, nullptr);
    
    LOGI("Performing TLS handshake...");
    int ret;
    while((ret = mbedtls_ssl_handshake(&g_ssl)) != 0) {
        if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOGE("TLS handshake failed: -0x%04x", -ret);
            return false;
        }
    }
    
    LOGI("TLS connection established securely");
    return true;
}

static bool handshake(mbedtls_ssl_context* ssl) {
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
    reg.object_value["token"] = SimpleJson(g_token);
    
    std::string str = reg.toString() + "\n";
    
    if(!send_all(ssl, str.c_str(), str.size())) return false;
    
    std::string line;
    char c;
    while(true) {
        if(!recv_all(ssl, &c, 1)) return false;
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

static void status_sender_thread(mbedtls_ssl_context* ssl) {
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
            if(send_frame(ssl, T_CALL_STATUS, json_str.c_str(), json_str.length())) {
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

static void capture_speaker_thread(mbedtls_ssl_context* ssl) {
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
            if(!send_frame(ssl, T_SPEAKER, pkt.data(), (uint32_t)len)) {
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

static void receive_virtual_mic_thread(mbedtls_ssl_context* ssl) {
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
        if(!recv_all(ssl, hdr, 5)) break;
        
        uint8_t type = hdr[0];
        uint32_t len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                       ((uint32_t)hdr[3] <<  8) | ((uint32_t)hdr[4]);
        
        if(len == 0 || len > MAX_PKT) break;
        if(!recv_all(ssl, pkt.data(), len)) break;
        
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
                send_json(ssl, T_PONG, pong);
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
    bool check_server = false;
    
    // Parse arguments
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--host") && i+1 < argc) g_host = argv[++i];
        else if(!strcmp(argv[i], "--port") && i+1 < argc) g_port = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--socket") && i+1 < argc) g_socket_path = argv[++i];
        else if(!strcmp(argv[i], "--token") && i+1 < argc) g_token = argv[++i];
        else if(!strcmp(argv[i], "--check-server")) check_server = true;
        else if(!strcmp(argv[i], "--daemon")) {
            // Daemonize
            if(fork() > 0) exit(0);
            setsid();
            fclose(stdin);
            fclose(stdout);
        }
    }
    
    // Load config from file if not provided via args
    FILE* f = fopen("/data/local/tmp/audio_bridge.conf", "r");
    if(f) {
        char line[256];
        while(fgets(line, sizeof(line), f)) {
            char key[64], val[192];
            if(sscanf(line, "%63[^=]=%191[^\n]", key, val) == 2) {
                if(!g_host && !strcmp(key, "HOST")) g_host = strdup(val);
                if(!strcmp(key, "PORT")) g_port = atoi(val);
                if(!strcmp(key, "TOKEN")) g_token = strdup(val);
            }
        }
        fclose(f);
    }
    
    if(!g_host) {
        fprintf(stderr, 
            "Usage: %s --host <VPS_IP> [--port N] [--socket PATH] [--token TOKEN] [--daemon] [--check-server]\n\n"
            "Audio Bridge v%d.%d.%d - Full telephony & SMS control (TLS Secured)\n"
            "Server commands available via TLS control channel.\n",
            argv[0], VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        return 1;
    }
    
    if(check_server) {
        LOGI("Checking TLS connection to %s:%d...", g_host, g_port);
        if(!tls_connect(g_host, g_port)) {
            LOGE("TLS Connection failed");
            return 1;
        }
        if(!handshake(&g_ssl)) {
            LOGE("Handshake/Auth failed");
            tls_cleanup();
            return 1;
        }
        LOGI("Connection and auth successful!");
        tls_cleanup();
        return 0; // Success
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
        LOGI("Connecting to %s:%d (TLS)...", g_host, g_port);
        
        if(!tls_connect(g_host, g_port)) {
            LOGW("TLS Connection failed, retrying in 15s");
            sleep(15);
            continue;
        }
        
        if(!handshake(&g_ssl)) {
            LOGW("Handshake/Auth failed, retrying in 15s");
            tls_cleanup();
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
        std::thread status_thread(status_sender_thread, &g_ssl);
        std::thread speaker_thread(capture_speaker_thread, &g_ssl);
        std::thread mic_thread(receive_virtual_mic_thread, &g_ssl);
        
        status_thread.join();
        speaker_thread.join();
        mic_thread.join();
        
        tls_cleanup();
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