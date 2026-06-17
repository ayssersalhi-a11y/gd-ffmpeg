#!/bin/bash
set -e

# ─── 0. التنظيف الشامل ────────────────────────────────────────────────────────
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
        wget -q --show-progress "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" -O "${TARBALL}"
        tar xzf "${TARBALL}" -C "${FFMPEG_SRC_DIR}" --strip-components=1
    fi
}

# ─── 2. دالة البناء (الاستراتيجية الموثوقة) ──────────────────────────────────
build_abi() {
    local ABI="$1"
    local ARCH="$2"
    local CPU="$3"
    local CROSS_PREFIX_BIN="$4"
    local PREFIX="${OUTPUT_DIR}/${ABI}"
    local TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
    local SYSROOT="${TOOLCHAIN}/sysroot"

    mkdir -p "${PREFIX}"

    # الخلطة السحرية: استخدام clang كمجمع (AS=CC) ودمج fPIC في cflags
    export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang"
    export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang++"
    export AS="${CC}"
    export AR="${TOOLCHAIN}/bin/llvm-ar"
    export NM="${TOOLCHAIN}/bin/llvm-nm"
    export STRIP="${TOOLCHAIN}/bin/llvm-strip"
    export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"

    cd "${FFMPEG_SRC_DIR}"
    make clean distclean 2>/dev/null || true

    ./configure \
        --prefix="${PREFIX}" \
        --target-os=android \
        --arch="${ARCH}" \
        --cpu="${CPU}" \
        --enable-cross-compile \
        --sysroot="${SYSROOT}" \
        --cc="${CC}" \
        --cxx="${CXX}" \
        --as="${AS}" \
        --ar="${AR}" \
        --nm="${NM}" \
        --ranlib="${RANLIB}" \
        --strip="${STRIP}" \
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
        --extra-cflags="-Os -fPIC -I${OPENSSL_BUILD}/include" \
        --extra-ldflags="-L${OPENSSL_BUILD}/lib" \
        --extra-libs="-lssl -lcrypto -lz"

    make -j"$(nproc)"
    make install
    cd ..
}

# ─── 3. التنفيذ ──────────────────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
API_LEVEL="${API_LEVEL:-24}"
mkdir -p "${OUTPUT_DIR}"
download_ffmpeg_source

build_abi "arm64-v8a" "aarch64" "armv8-a" "aarch64-linux-android"

echo "✅ تم البناء بنجاح كامل!"
