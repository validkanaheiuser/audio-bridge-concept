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
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_PHONE_CALL" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MICROPHONE" />
    <uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW" />

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

    # Build APK using gradle
    cat > app/build.gradle << 'EOF'
buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        classpath 'com.android.tools.build:gradle:8.1.0'
    }
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

repositories {
    google()
    mavenCentral()
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
        -I"$PROJECT_DIR/zygisk" \
        src/zygisk_module.cpp \
        -o "$PROJECT_DIR/zygisk/module/zygisk/arm64-v8a.so" \
        -llog
    
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
set_perm_recursive $MODPATH/system/priv-app/AudioBridge 0 0 0755 0644
set_perm $MODPATH/system/bin/audio-bridge 0 2000 0755
EOF

    # Create service.sh
    cat > "$PROJECT_DIR/zygisk/module/service.sh" << 'EOF'
#!/system/bin/sh
MODDIR=${0%/*}

# Auto-grant permissions
pm grant com.audiobridge android.permission.CALL_PHONE
pm grant com.audiobridge android.permission.ANSWER_PHONE_CALLS
pm grant com.audiobridge android.permission.READ_PHONE_STATE
pm grant com.audiobridge android.permission.SEND_SMS
pm grant com.audiobridge android.permission.RECEIVE_SMS
pm grant com.audiobridge android.permission.READ_SMS
appops set com.audiobridge SYSTEM_ALERT_WINDOW allow

# Start daemon if not running
if [ ! -f /data/local/tmp/audio_bridge.pid ] || ! kill -0 $(cat /data/local/tmp/audio_bridge.pid) 2>/dev/null; then
    $MODDIR/system/bin/audio-bridge --daemon &
fi
EOF
    chmod +x "$PROJECT_DIR/zygisk/module/service.sh"

    # Create post-fs-data.sh
    cat > "$PROJECT_DIR/zygisk/module/post-fs-data.sh" << 'EOF'
#!/system/bin/sh
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
    
    # Try to build APK if gradle is available
    echo -e "${YELLOW}Attempting to build APK using gradle...${NC}"
    if command -v gradle >/dev/null 2>&1; then
        cd "$PROJECT_DIR/app" && gradle assembleRelease || echo "Gradle build failed"
    elif [ -f "$PROJECT_DIR/app/gradlew" ]; then
        cd "$PROJECT_DIR/app" && chmod +x gradlew && ./gradlew assembleRelease || echo "Gradlew build failed"
    else
        echo -e "${RED}Gradle not found. Please build the APK manually in the 'app' directory, then re-run this script.${NC}"
    fi
    cd "$PROJECT_DIR"
    
    # Build Zygisk module
    build_zygisk
    
    # Package APK and binary into module
    APK_PATH=$(find "$PROJECT_DIR/app/build/outputs/apk/release" -name "*.apk" | head -n 1)
    if [ -n "$APK_PATH" ] && [ -f "$APK_PATH" ]; then
        cp "$APK_PATH" "$PROJECT_DIR/zygisk/module/system/priv-app/AudioBridge/AudioBridge.apk"
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
