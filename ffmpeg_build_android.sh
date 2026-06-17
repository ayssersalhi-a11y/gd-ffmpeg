#!/bin/bash
set -e

# ─── 0. التنظيف الشامل لضمان بيئة بناء نظيفة ────────────────────────────────
echo "🧹 تنظيف ملفات البناء القديمة..."
rm -rf ffmpeg_build
rm -rf _cmake_build
if [ -d "ffmpeg_source" ]; then
    cd ffmpeg_source
    make distclean || true
    cd ..
fi

# ─── 1. الإعدادات والمسارات ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg_source"
OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build"
OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"

# ─── دالة مشتركة: تحميل FFmpeg المصدري ───────────────────────────────────────
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
    else
        echo "✓ كود FFmpeg موجود بالفعل"
    fi
}

# ─── 2. بناء Linux ───────────────────────────────────────────────────────────
if [ "$TARGET_PLATFORM" = "linux" ]; then
    echo "=================================================="
    echo "⚙️  بناء FFmpeg لـ Linux x86_64"
    echo "=================================================="

    mkdir -p "${OUTPUT_DIR}"
    download_ffmpeg_source

    cd "${FFMPEG_SRC_DIR}"
    make clean 2>/dev/null || true
    
    export ASFLAGS="-fPIC"
    export CFLAGS="-fPIC -I${OPENSSL_BUILD}/include"
    export CXXFLAGS="-fPIC"
    
    ./configure \
        --prefix="${OUTPUT_DIR}" \
        --enable-static --disable-shared --disable-programs --disable-doc \
        --enable-pic \
        --enable-openssl \
        --extra-ldflags="-L${OPENSSL_BUILD}/lib" \
        --extra-libs="-lssl -lcrypto -lz"

    make -j"$(nproc)"
    make install
    echo "✅ تم بناء FFmpeg للينكس بنجاح!"
    exit 0
fi

# ─── 3. إعدادات الأندرويد ─────────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
API_LEVEL="${API_LEVEL:-24}"

echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║  بناء FFmpeg لـ Android (OpenSSL + MediaCodec)         ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""

[ ! -d "${NDK_PATH}" ] && { echo "✗ NDK مفقود"; exit 1; }
[ ! -f "${OPENSSL_BUILD}/lib/libssl.a" ] && { echo "✗ OpenSSL مفقود"; exit 1; }

mkdir -p "${OUTPUT_DIR}"
download_ffmpeg_source

# ─── 4. دالة البناء للأندرويد ────────────────────────────────────────────────
build_abi() {
    local ABI="$1"
    local ARCH="$2"
    local CPU="$3"
    local CROSS_PREFIX_BIN="$4"
    local PREFIX="${OUTPUT_DIR}/${ABI}"

    mkdir -p "${PREFIX}"
    local TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"

    export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang"
    export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang++"
    
    # تعريف الـ Flags لضمان الـ PIC في كل جزء من عملية البناء
    export ASFLAGS="-fPIC"
    export CFLAGS="-fPIC -Os -I${OPENSSL_BUILD}/include"
    export CXXFLAGS="-fPIC"

    cd "${FFMPEG_SRC_DIR}"
    make clean distclean 2>/dev/null || true

    ./configure \
        --prefix="${PREFIX}" --target-os=android --arch="${ARCH}" --cpu="${CPU}" \
        --enable-cross-compile --sysroot="${TOOLCHAIN}/sysroot" \
        --cc="${CC}" --cxx="${CXX}" --enable-static --disable-shared \
        --disable-programs --disable-doc --disable-everything \
        --enable-pic \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
        --enable-jni --enable-mediacodec \
        --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg4,aac,mp3,opus,flac,ac3 \
        --enable-demuxer=mp4,matroska,mov,avi,hls,concat,mp3,ogg,aac,flac,wav,mpegts \
        --enable-parser=h264,hevc,aac,opus,mp3,flac \
        --enable-protocol=file,pipe,http,https,hls,tcp,tls,ssl,crypto,data,ftp \
        --enable-openssl \
        --extra-ldflags="-L${OPENSSL_BUILD}/lib" \
        --extra-libs="-lssl -lcrypto -lz"

    make -j"$(nproc)"
    make install
}

build_abi "arm64-v8a" "aarch64" "armv8-a" "aarch64-linux-android"

echo "✅ تم البناء بنجاح!"
