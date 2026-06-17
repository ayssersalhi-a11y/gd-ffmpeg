#!/bin/bash
set -e

# ─── 1. الإعدادات والمسارات ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg_source"
OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build"
OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build"

# ─── 2. بناء الأندرويد ──────────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"
API_LEVEL="${API_LEVEL:-24}"

# دالة البناء
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
    
    cd "${FFMPEG_SRC_DIR}"
    make clean distclean 2>/dev/null || true

    ./configure \
        --prefix="${PREFIX}" --target-os=android --arch="${ARCH}" --cpu="${CPU}" \
        --enable-cross-compile --sysroot="${TOOLCHAIN}/sysroot" \
        --cc="${CC}" --cxx="${CXX}" --enable-static --disable-shared \
        --disable-programs --disable-doc --disable-everything \
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
}

# تحميل المصدر أولاً إذا لم يوجد
if [ ! -d "${FFMPEG_SRC_DIR}" ]; then
    mkdir -p "${FFMPEG_SRC_DIR}"
    wget -q --show-progress "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" -O ffmpeg.tar.gz
    tar xzf ffmpeg.tar.gz -C "${FFMPEG_SRC_DIR}" --strip-components=1
fi

build_abi "arm64-v8a" "aarch64" "armv8-a" "aarch64-linux-android"

echo "✅ تم البناء بنجاح!"
