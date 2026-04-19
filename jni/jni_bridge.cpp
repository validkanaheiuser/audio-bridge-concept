/**
 * JNI Bridge - Audio Bridge
 * JNI method registration and native callback implementations
 * This file is compiled into libaudiobridge.so
 */

#include <jni.h>
#include <string>
#include <android/log.h>
#include "audio_bridge.h"

#define LOG_TAG "AudioBridge-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM* g_jvm = nullptr;

extern "C" {

// ──────────────────────────────────────────────────────────────────────────
// JNI Lifecycle
// ──────────────────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: Audio Bridge native library loaded");
    return JNI_VERSION_1_6;
}

// ──────────────────────────────────────────────────────────────────────────
// Native callbacks from Java TelephonyHelper
// ──────────────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnCallStateChanged(
    JNIEnv* env, jobject thiz, jint state, jstring number) {
    
    const char* numStr = env->GetStringUTFChars(number, nullptr);
    LOGI("Call state changed: %d, number: %s", state, numStr ? numStr : "");
    env->ReleaseStringUTFChars(number, numStr);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnCallWaiting(
    JNIEnv* env, jobject thiz, jstring incomingNumber, jstring currentNumber) {
    
    const char* incoming = env->GetStringUTFChars(incomingNumber, nullptr);
    const char* current = env->GetStringUTFChars(currentNumber, nullptr);
    LOGI("Call waiting: incoming=%s, current=%s", 
         incoming ? incoming : "?", current ? current : "?");
    env->ReleaseStringUTFChars(incomingNumber, incoming);
    env->ReleaseStringUTFChars(currentNumber, current);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnSMSSent(
    JNIEnv* env, jobject thiz, jstring messageId, jint resultCode) {
    
    const char* id = env->GetStringUTFChars(messageId, nullptr);
    LOGI("SMS sent: %s, result=%d", id ? id : "?", resultCode);
    env->ReleaseStringUTFChars(messageId, id);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnSMSDelivered(
    JNIEnv* env, jobject thiz, jstring messageId) {
    
    const char* id = env->GetStringUTFChars(messageId, nullptr);
    LOGI("SMS delivered: %s", id ? id : "?");
    env->ReleaseStringUTFChars(messageId, id);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_TelephonyHelper_nativeOnSMSReceived(
    JNIEnv* env, jobject thiz, jstring sender, jstring message, jlong timestamp) {
    
    const char* from = env->GetStringUTFChars(sender, nullptr);
    const char* msg = env->GetStringUTFChars(message, nullptr);
    LOGI("SMS received from %s: %s", from ? from : "?", msg ? msg : "");
    env->ReleaseStringUTFChars(sender, from);
    env->ReleaseStringUTFChars(message, msg);
}

} // extern "C"
