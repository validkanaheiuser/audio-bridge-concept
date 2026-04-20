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

# Build mbedtls
build_mbedtls() {
    local ARCH=$1
    local ABI=$2
    local TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
    
    echo -e "${YELLOW}Building mbedtls for $ARCH...${NC}"
    
    cd "$BUILD_DIR"
    if [ ! -d "mbedtls" ]; then
        git clone --depth 1 -b v3.6.0 https://github.com/Mbed-TLS/mbedtls.git
        cd mbedtls
        git submodule update --init
    else
        cd mbedtls
    fi
    
    export CC="$TOOLCHAIN/bin/${ARCH}-linux-android${API_LEVEL}-clang"
    export AR="$TOOLCHAIN/bin/llvm-ar"
    
    # Simple makefile build for mbedtls
    make clean 2>/dev/null || true
    make lib CC="$CC" AR="$AR" CFLAGS="-O3 -fPIC" -j$(nproc)
    
    mkdir -p "$LIBS_DIR/$ABI/include/mbedtls"
    mkdir -p "$LIBS_DIR/$ABI/include/psa"
    cp library/libmbedcrypto.a library/libmbedtls.a library/libmbedx509.a "$LIBS_DIR/$ABI/"
    cp -r include/mbedtls/* "$LIBS_DIR/$ABI/include/mbedtls/"
    cp -r include/psa/* "$LIBS_DIR/$ABI/include/psa/"
    
    echo -e "${GREEN}mbedtls built for $ARCH${NC}"
}

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
    
    mkdir -p "$LIBS_DIR/$ABI/include"
    cp -r "$BUILD_DIR/opus-install-$ABI/include/opus" "$LIBS_DIR/$ABI/include/"
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
    make -C src libtinyalsa.a \
        CC="$CC" \
        AR="$AR" \
        CFLAGS="-O3 -fPIC -DANDROID"
    
    mkdir -p "$LIBS_DIR/$ABI/include/tinyalsa"
    cp src/libtinyalsa.a "$LIBS_DIR/$ABI/libtinyalsa.a" || cp libtinyalsa.a "$LIBS_DIR/$ABI/libtinyalsa.a"
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
        -I"$LIBS_DIR/$ABI/include" \
        -I"$PROJECT_DIR/jni" \
        audio_bridge.cpp \
        -o "$BUILD_DIR/audio-bridge-$ABI" \
        -L"$LIBS_DIR/$ABI" \
        -lopus \
        -ltinyalsa \
        -lmbedtls -lmbedx509 -lmbedcrypto \
        -static-libstdc++ \
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
    cp java/com/audiobridge/AudioBridgeService.java app/src/main/java/com/audiobridge/
    cp java/com/audiobridge/IPCClient.java app/src/main/java/com/audiobridge/
    cp java/com/audiobridge/BootReceiver.java app/src/main/java/com/audiobridge/
    
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
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_DATA_SYNC" />
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
    <uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW" />

    <application
        android:label="@string/app_name"
        android:icon="@android:drawable/ic_menu_call"
        android:theme="@android:style/Theme.DeviceDefault.NoActionBar"
        android:allowBackup="false"
        android:supportsRtl="true"
        android:usesCleartextTraffic="true"
        android:networkSecurityConfig="@xml/network_security_config"
        android:localeConfig="@xml/locales_config">

        <service
            android:name=".AudioBridgeService"
            android:enabled="true"
            android:exported="true"
            android:foregroundServiceType="dataSync" />

        <receiver
            android:name=".BootReceiver"
            android:enabled="true"
            android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.BOOT_COMPLETED" />
                <action android:name="com.audiobridge.START" />
            </intent-filter>
        </receiver>
    </application>
</manifest>
EOF

    # Create strings.xml. The english default ensures updateLocaleListFromAppContext
    # has something to resolve — Android 14 NPE's in handleBindApplication if
    # resources are under-specified.
    cat > app/src/main/res/values/strings.xml << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">Audio Bridge</string>
    <string name="notification_title">Audio Bridge</string>
    <string name="notification_text">Running in background</string>
</resources>
EOF

    # locale_config referenced by @string lookups during bind-application.
    # Without this, priv-app scanning on API 34 has been observed to init
    # Resources as null and crash with getResources().getConfiguration() NPE.
    mkdir -p app/src/main/res/xml
    cat > app/src/main/res/xml/locales_config.xml << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<locale-config xmlns:android="http://schemas.android.com/apk/res/android">
    <locale android:name="en"/>
</locale-config>
EOF

    cat > app/src/main/res/xml/network_security_config.xml << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<network-security-config>
    <base-config cleartextTrafficPermitted="true"/>
</network-security-config>
EOF

    # Copy native library
    mkdir -p app/src/main/jniLibs/arm64-v8a
    mkdir -p app/src/main/jniLibs/armeabi-v7a
    cp "$BUILD_DIR/libaudiobridge-arm64-v8a.so" app/src/main/jniLibs/arm64-v8a/libaudiobridge.so 2>/dev/null || true
    
    cat > app/settings.gradle << 'EOF'
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
rootProject.name = "AudioBridge"
EOF

    # local.properties: required so gradle can locate the Android SDK
    cat > app/local.properties << EOF
sdk.dir=${ANDROID_HOME}
EOF

    # gradle.properties: defaults for headless builds
    cat > app/gradle.properties << 'EOF'
android.useAndroidX=true
android.nonTransitiveRClass=true
org.gradle.jvmargs=-Xmx2048m
EOF

    # Generate a release keystore if none exists. Without this, gradle emits
    # an unsigned APK and Android rejects it with INSTALL_PARSE_FAILED_NO_CERTIFICATES.
    # Override with env var AUDIO_BRIDGE_KEYSTORE to use your own signing key.
    local KEYSTORE_PATH="${AUDIO_BRIDGE_KEYSTORE:-$PROJECT_DIR/app/audiobridge.keystore}"
    local KEYSTORE_PASS="${AUDIO_BRIDGE_KEYSTORE_PASS:-audiobridge}"
    local KEYSTORE_ALIAS="${AUDIO_BRIDGE_KEYSTORE_ALIAS:-audiobridge}"
    if [ ! -f "$KEYSTORE_PATH" ]; then
        echo -e "${YELLOW}Generating signing keystore: $KEYSTORE_PATH${NC}"
        keytool -genkeypair -noprompt \
            -keystore "$KEYSTORE_PATH" \
            -alias "$KEYSTORE_ALIAS" \
            -storepass "$KEYSTORE_PASS" \
            -keypass "$KEYSTORE_PASS" \
            -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=AudioBridge, O=AudioBridge, C=US" \
        || echo -e "${RED}keytool failed — ensure Java JDK is installed${NC}"
    fi

    cat > app/build.gradle << EOF
buildscript {
    repositories { google(); mavenCentral() }
    dependencies { classpath 'com.android.tools.build:gradle:8.1.0' }
}

apply plugin: 'com.android.application'

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

    signingConfigs {
        release {
            storeFile file("${KEYSTORE_PATH}")
            storePassword "${KEYSTORE_PASS}"
            keyAlias "${KEYSTORE_ALIAS}"
            keyPassword "${KEYSTORE_PASS}"
            // Enable both v1 (JAR) and v2+ (APK Signature Scheme) so the APK
            // is accepted on the full min/target SDK range.
            v1SigningEnabled true
            v2SigningEnabled true
        }
    }

    buildTypes {
        release {
            minifyEnabled false
            signingConfig signingConfigs.release
        }
    }

    compileOptions {
        sourceCompatibility JavaVersion.VERSION_1_8
        targetCompatibility JavaVersion.VERSION_1_8
    }
}

repositories { google(); mavenCentral() }
EOF

    echo -e "${GREEN}APK source prepared (signing via $KEYSTORE_PATH)${NC}"
}

# Fetch shadowhook from Maven Central. The AAR bundles prebuilt libshadowhook.so
# for every ABI plus the public header. Drastically simpler than building from
# source, and avoids Dobby's master-branch breakage (OSMemory/RuntimeModule,
# 2026-Q2).
build_shadowhook() {
    local VERSION=1.0.10
    local AAR_URL="https://repo1.maven.org/maven2/com/bytedance/android/shadowhook/${VERSION}/shadowhook-${VERSION}.aar"
    echo -e "${YELLOW}Fetching shadowhook ${VERSION} prebuilt...${NC}"

    cd "$BUILD_DIR"
    if [ ! -f "shadowhook-${VERSION}.aar" ]; then
        curl -sL -o "shadowhook-${VERSION}.aar" "$AAR_URL"
    fi

    rm -rf "$BUILD_DIR/shadowhook-aar"
    mkdir -p "$BUILD_DIR/shadowhook-aar"
    unzip -q -o "shadowhook-${VERSION}.aar" -d "$BUILD_DIR/shadowhook-aar"

    for ABI in arm64-v8a armeabi-v7a; do
        mkdir -p "$LIBS_DIR/$ABI/include/shadowhook"
        cp "$BUILD_DIR/shadowhook-aar/jni/$ABI/libshadowhook.so" "$LIBS_DIR/$ABI/" || \
            echo -e "${RED}shadowhook: missing .so for $ABI${NC}"
        cp "$BUILD_DIR/shadowhook-aar/prefab/modules/shadowhook/include/shadowhook.h" \
            "$LIBS_DIR/$ABI/include/shadowhook/"
    done
    echo -e "${GREEN}shadowhook ${VERSION} ready${NC}"
}

# Build Zygisk module
build_zygisk() {
    echo -e "${YELLOW}Building Zygisk module...${NC}"

    # Ensure shadowhook is available
    if [ ! -f "$LIBS_DIR/arm64-v8a/libshadowhook.so" ]; then
        build_shadowhook
    fi

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

    # Compile Zygisk module. We don't link against libshadowhook.so — the
    # module dlopens it at runtime from its own install dir (see src/) so we
    # don't need to arrange for the dynamic linker to find it.
    $CXX \
        -std=c++17 \
        -O3 \
        -fPIC \
        -shared \
        -DANDROID \
        -I"$PROJECT_DIR/zygisk" \
        -I"$LIBS_DIR/arm64-v8a/include" \
        -I"$LIBS_DIR/arm64-v8a/include/shadowhook" \
        src/zygisk_module.cpp \
        -o "$PROJECT_DIR/zygisk/module/zygisk/arm64-v8a.so" \
        -Wl,--gc-sections \
        -ldl \
        -llog

    # Ship libshadowhook.so alongside our zygisk module
    cp "$LIBS_DIR/arm64-v8a/libshadowhook.so" "$PROJECT_DIR/zygisk/module/zygisk/"
    
    # Package into Magisk Module
    mkdir -p "$PROJECT_DIR/zygisk/module/system/priv-app/AudioBridge"
    mkdir -p "$PROJECT_DIR/zygisk/module/system/etc/permissions"
    mkdir -p "$PROJECT_DIR/zygisk/module/system/bin"
    
    # Create privapp-permissions.xml
    cat > "$PROJECT_DIR/zygisk/module/system/etc/permissions/privapp-permissions-audiobridge.xml" << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<permissions>
    <privapp-permissions package="com.audiobridge">
        <permission name="android.permission.CALL_PHONE"/>
        <permission name="android.permission.ANSWER_PHONE_CALLS"/>
        <permission name="android.permission.READ_PHONE_STATE"/>
        <permission name="android.permission.SEND_SMS"/>
        <permission name="android.permission.RECEIVE_SMS"/>
        <permission name="android.permission.READ_SMS"/>
        <permission name="android.permission.SYSTEM_ALERT_WINDOW"/>
    </privapp-permissions>
</permissions>
EOF

    # Create module.prop
    cat > "$PROJECT_DIR/zygisk/module/module.prop" << EOF
id=audio_bridge
name=Audio Bridge
version=v3.0
versionCode=300
author=AudioBridge
description=Virtual microphone and speaker capture for remote audio. Plug-and-play with TLS.
EOF

    # Create customize.sh
    cat > "$PROJECT_DIR/zygisk/module/customize.sh" << 'EOF'
ui_print "- Installing Audio Bridge"
ui_print "- Android 14 Compatible"
EOF

    # Create sepolicy.rule for every (app, daemon) domain pair we might hit.
    # KernelSU's service.sh runs the daemon in `ksu`; Magisk puts it in `magisk`
    # or `su`. The app is `priv_app` (priv-app install) or `system_app`; the
    # Phone process is `radio`. Without this, LocalSocket.connect() gets an
    # EACCES via SELinux and `am broadcast` succeeds but IPC silently fails.
    cat > "$PROJECT_DIR/zygisk/module/sepolicy.rule" << 'EOF'
allow priv_app   ksu     unix_stream_socket { connectto read write getattr }
allow priv_app   magisk  unix_stream_socket { connectto read write getattr }
allow priv_app   su      unix_stream_socket { connectto read write getattr }
allow priv_app   init    unix_stream_socket { connectto read write getattr }
allow system_app ksu     unix_stream_socket { connectto read write getattr }
allow system_app magisk  unix_stream_socket { connectto read write getattr }
allow system_app su      unix_stream_socket { connectto read write getattr }
allow platform_app ksu   unix_stream_socket { connectto read write getattr }
allow platform_app magisk unix_stream_socket { connectto read write getattr }
allow platform_app su    unix_stream_socket { connectto read write getattr }
allow radio      ksu     unix_stream_socket { connectto read write getattr }
allow radio      magisk  unix_stream_socket { connectto read write getattr }
allow radio      su      unix_stream_socket { connectto read write getattr }
EOF

    # Create service.sh (runs during late_start - safe, non-blocking)
    cat > "$PROJECT_DIR/zygisk/module/service.sh" << 'EOF'
#!/system/bin/sh
MODDIR=${0%/*}
LOG=/data/local/tmp/audio_bridge_service.log

echo "$(date) Audio Bridge service.sh started" >> $LOG

# Auto-grant permissions (suppress errors if app not yet installed)
pm grant com.audiobridge android.permission.CALL_PHONE 2>/dev/null
pm grant com.audiobridge android.permission.ANSWER_PHONE_CALLS 2>/dev/null
pm grant com.audiobridge android.permission.READ_PHONE_STATE 2>/dev/null
pm grant com.audiobridge android.permission.SEND_SMS 2>/dev/null
pm grant com.audiobridge android.permission.RECEIVE_SMS 2>/dev/null
pm grant com.audiobridge android.permission.READ_SMS 2>/dev/null
pm grant com.audiobridge android.permission.POST_NOTIFICATIONS 2>/dev/null
appops set com.audiobridge SYSTEM_ALERT_WINDOW allow 2>/dev/null

# Apply SELinux rules. sepolicy.rule is read by Magisk/KernelSU on boot; this
# is the belt to that file's suspenders. Rules cover every (app, daemon)
# domain pair we might hit.
APP_DOMAINS="priv_app system_app platform_app radio"
DAEMON_DOMAINS="ksu magisk su init"
if command -v magiskpolicy >/dev/null 2>&1; then
    for APP in $APP_DOMAINS; do for D in $DAEMON_DOMAINS; do
        magiskpolicy --live "allow $APP $D unix_stream_socket { connectto read write getattr }" 2>/dev/null
    done; done
    echo "$(date) SELinux rules applied via magiskpolicy" >> $LOG
elif [ -f /data/adb/ksud ]; then
    for APP in $APP_DOMAINS; do for D in $DAEMON_DOMAINS; do
        /data/adb/ksud apply-sepolicy "allow $APP $D unix_stream_socket { connectto read write getattr }" 2>/dev/null
    done; done
    echo "$(date) SELinux rules applied via ksud" >> $LOG
elif command -v supolicy >/dev/null 2>&1; then
    for APP in $APP_DOMAINS; do for D in $DAEMON_DOMAINS; do
        supolicy --live "allow $APP $D unix_stream_socket { connectto read write getattr }" 2>/dev/null
    done; done
    echo "$(date) SELinux rules applied via supolicy" >> $LOG
else
    echo "$(date) WARNING: no sepolicy tool found" >> $LOG
fi

# Start daemon if not running
if ! pidof audio-bridge >/dev/null 2>&1; then
    echo "$(date) Starting audio bridge daemon" >> $LOG
    if [ -f /system/bin/audio-bridge ]; then
        /system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    else
        echo "$(date) /system/bin/audio-bridge not found, trying MODDIR" >> $LOG
        chmod 755 $MODDIR/system/bin/audio-bridge 2>/dev/null
        $MODDIR/system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    fi
    sleep 3
    if pidof audio-bridge >/dev/null 2>&1; then
        echo "$(date) Daemon started, PID: $(pidof audio-bridge)" >> $LOG
    else
        echo "$(date) WARNING: Daemon failed to start" >> $LOG
    fi
else
    echo "$(date) Daemon already running, PID: $(pidof audio-bridge)" >> $LOG
fi

# Background: wait for the framework, install APK if needed, start service.
(
    # Wait for boot_completed (cap ~2 min).
    for i in $(seq 1 60); do
        if [ "$(getprop sys.boot_completed)" = "1" ]; then break; fi
        sleep 2
    done
    sleep 3

    # Fallback install: if priv-app overlay didn't register com.audiobridge
    # (e.g. signing mismatch on restrictive ROMs), pm install from MODDIR.
    if ! pm path com.audiobridge >/dev/null 2>&1; then
        if [ -f "$MODDIR/AudioBridge.apk" ]; then
            echo "$(date) com.audiobridge not registered as priv-app; pm install fallback" >> $LOG
            pm install -r -g "$MODDIR/AudioBridge.apk" >> $LOG 2>&1
        else
            echo "$(date) WARNING: AudioBridge.apk missing from module" >> $LOG
        fi
    else
        echo "$(date) com.audiobridge present at $(pm path com.audiobridge)" >> $LOG
    fi

    # Start the foreground service — retry because activity may still be
    # registering a beat after boot_completed on some devices.
    # Trigger service start via our custom broadcast. Broadcasts run in the
    # app's own UID so they're exempt from Android 12+'s BG-FGS guard that
    # makes `am start-foreground-service` fail with "app is in background
    # uid null" when fired from the shell.
    for i in 1 2 3 4 5; do
        OUT=$(am broadcast --user 0 -a com.audiobridge.START \
              -n com.audiobridge/.BootReceiver 2>&1)
        echo "$(date) broadcast try $i: $OUT" >> $LOG
        if echo "$OUT" | grep -q "result=0"; then
            echo "$(date) AudioBridgeService start broadcast delivered" >> $LOG
            exit 0
        fi
        sleep 3
    done
    echo "$(date) WARNING: START broadcast never delivered" >> $LOG
) &
EOF
    chmod +x "$PROJECT_DIR/zygisk/module/service.sh"

    # Create post-fs-data.sh (MUST be minimal - never block boot)
    cat > "$PROJECT_DIR/zygisk/module/post-fs-data.sh" << 'EOF'
#!/system/bin/sh
# Keep empty to avoid blocking boot
MODDIR=${0%/*}
EOF
    chmod +x "$PROJECT_DIR/zygisk/module/post-fs-data.sh"
    
    echo -e "${GREEN}Zygisk module built${NC}"
}

# Build all
main() {
    echo -e "${YELLOW}Starting full build...${NC}"
    
    # Build mbedtls
    build_mbedtls "aarch64" "arm64-v8a"
    
    # Build dependencies for arm64
    build_opus "aarch64" "arm64-v8a"
    build_tinyalsa "aarch64" "arm64-v8a"
    
    # Build native binary
    build_native "aarch64" "arm64-v8a"

    # Prepare APK sources
    build_apk
    
    # Try to build APK if gradle is available. Generate a wrapper once so
    # subsequent builds are self-contained.
    echo -e "${YELLOW}Attempting to build APK using gradle...${NC}"
    if [ ! -f "$PROJECT_DIR/app/gradlew" ] && command -v gradle >/dev/null 2>&1; then
        echo -e "${YELLOW}Generating gradle wrapper...${NC}"
        ( cd "$PROJECT_DIR/app" && gradle wrapper --gradle-version 8.4 ) || \
            echo -e "${RED}Failed to generate gradle wrapper${NC}"
    fi

    if [ -f "$PROJECT_DIR/app/gradlew" ]; then
        ( cd "$PROJECT_DIR/app" && chmod +x gradlew && ./gradlew assembleRelease --no-daemon ) || \
            echo -e "${RED}Gradlew build failed${NC}"
    elif command -v gradle >/dev/null 2>&1; then
        ( cd "$PROJECT_DIR/app" && gradle assembleRelease --no-daemon ) || \
            echo -e "${RED}Gradle build failed${NC}"
    else
        echo -e "${RED}Gradle not found. Install gradle (>=8) and re-run, or open app/ in Android Studio.${NC}"
    fi
    cd "$PROJECT_DIR"
    
    # Build Zygisk module
    build_zygisk
    
    # Package APK and binary into module. We ship two copies:
    #   1. system/priv-app/AudioBridge/AudioBridge.apk — Magisk systemless overlay
    #      makes /system/priv-app/ include the APK; Android picks it up on boot
    #      scan with privileged permissions (privapp-permissions-audiobridge.xml).
    #   2. AudioBridge.apk at module root — service.sh falls back to `pm install`
    #      from here if priv-app detection didn't pick up the package (e.g. on
    #      restrictive ROMs or when signing requirements differ).
    APK_PATH=$(find "$PROJECT_DIR/app/build/outputs/apk" -name "*.apk" 2>/dev/null | head -n 1)
    if [ -n "$APK_PATH" ] && [ -f "$APK_PATH" ]; then
        cp "$APK_PATH" "$PROJECT_DIR/zygisk/module/system/priv-app/AudioBridge/AudioBridge.apk"
        cp "$APK_PATH" "$PROJECT_DIR/zygisk/module/AudioBridge.apk"
        echo -e "${GREEN}Packaged APK: $APK_PATH${NC}"
    else
        echo -e "${RED}Warning: APK not found! Module will not be fully functional until APK is placed in zygisk/module/system/priv-app/AudioBridge/${NC}"
    fi
    
    cp "$BUILD_DIR/audio-bridge-arm64-v8a" "$PROJECT_DIR/zygisk/module/system/bin/audio-bridge"
    
    # Zip module
    cd "$PROJECT_DIR/zygisk/module"
    zip -r9 "$PROJECT_DIR/build/audio-bridge-module.zip" ./*
    cd "$PROJECT_DIR"
    
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                    Build Complete!                           ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "Output files:"
    echo -e "  Magisk/KernelSU Module: ${YELLOW}$PROJECT_DIR/build/audio-bridge-module.zip${NC}"
    echo ""
    echo -e "Installation:"
    echo -e "  Flash the module zip in Magisk or KernelSU and reboot."
}

main "$@"
