#!/bin/bash
set -e

# ─── 0. التنظيف الشامل لضمان بيئة بناء نظيفة ────────────────────────────────
echo "🧹 تنظيف ملفات البناء القديمة..."
rm -rf ffmpeg_build
rm -rf _cmake_build
if [ -d "ffmpeg_source" ]; then
    cd ffmpeg_source
    make distclean 2>/dev/null || true
    cd ..
fi

# ─── 1. الإعدادات والمسارات ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg_source"
OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build"
OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"

download_ffmpeg_source() {
    mkdir -p "${FFMPEG_SRC_DIR}"
    if [ ! -f "${FFMPEG_SRC_DIR}/configure" ]; then
        echo "── تحميل FFmpeg ${FFMPEG_VERSION} ──"
        local TARBALL="${SCRIPT_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz"
        wget -q --show-progress \
            "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" \
            -O "${TARBALL}"
        tar xzf "${TARBALL}" -C "${FFMPEG_SRC_DIR}" --strip-components=1
        echo "✓ تم استخراج FFmpeg ${FFMPEG_VERSION}"
    fi
}

# ─── 2. بناء Linux ───────────────────────────────────────────────────────────
if [ "$TARGET_PLATFORM" = "linux" ]; then
    mkdir -p "${OUTPUT_DIR}"
    download_ffmpeg_source
    cd "${FFMPEG_SRC_DIR}"
    make clean 2>/dev/null || true
    
    export ASFLAGS="-fPIC"
    export CFLAGS="-fPIC -I${OPENSSL_BUILD}/include"
    export CXXFLAGS="-fPIC"
    
    ./configure --prefix="${OUTPUT_DIR}" --enable-static --disable-shared --disable-programs --enable-pic --enable-openssl --extra-ldflags="-L${OPENSSL_BUILD}/lib" --extra-libs="-lssl -lcrypto -lz"
    make -j"$(nproc)" && make install
    exit 0
fi

# ─── 3. بناء Android (ARM64) ────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
API_LEVEL="${API_LEVEL:-24}"

mkdir -p "${OUTPUT_DIR}"
download_ffmpeg_source

# إعدادات الـ Toolchain
ABI="arm64-v8a"
ARCH="aarch64"
CROSS_PREFIX="aarch64-linux-android"
TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
PREFIX="${OUTPUT_DIR}/${ABI}"

export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX}${API_LEVEL}-clang"
export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX}${API_LEVEL}-clang++"
export AS="${TOOLCHAIN}/bin/${CROSS_PREFIX}${API_LEVEL}-clang -fPIC"

# تصدير الـ Flags كمتغيرات بيئية
export ASFLAGS="-fPIC"
export CFLAGS="-fPIC -Os -I${OPENSSL_BUILD}/include"
export CXXFLAGS="-fPIC"

cd "${FFMPEG_SRC_DIR}"
make clean distclean 2>/dev/null || true

# تشغيل configure 
./configure \
    --prefix="${PREFIX}" \
    --target-os=android \
    --arch="${ARCH}" \
    --cpu="armv8-a" \
    --enable-cross-compile \
    --sysroot="${TOOLCHAIN}/sysroot" \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --as="${AS}" \
    --enable-static \
    --disable-shared \
    --disable-programs \
    --disable-doc \
    --disable-everything \
    --enable-pic \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
    --enable-jni --enable-mediacodec \
    --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg4,aac,mp3,opus,flac,ac3 \
    --enable-demuxer=mp4,matroska,mov,avi,hls,concat,mp3,ogg,aac,flac,wav,mpegts \
    --enable-parser=h264,hevc,aac,opus,mp3,flac \
    --enable-protocol=file,pipe,http,https,hls,tcp,tls,ssl,crypto,data,ftp \
    --enable-openssl \
    --extra-cflags="${CFLAGS}" \
    --extra-cxxflags="${CXXFLAGS}" \
    --extra-ldflags="-L${OPENSSL_BUILD}/lib" \
    --extra-libs="-lssl -lcrypto -lz"

# [الحقنة الأقوى]: فرض -fPIC في ملفات الـ Makefile مباشرة
echo "💉 حقن -fPIC في ملفات البناء (config.mak)..."
sed -i 's/ASFLAGS=/ASFLAGS=-fPIC /g' config.mak
sed -i 's/CFLAGS=/CFLAGS=-fPIC /g' config.mak

make -j"$(nproc)"
make install

echo "✅ تم بناء FFmpeg للأندرويد بنجاح كامل مع فرض PIC!"
