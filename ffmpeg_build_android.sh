#!/bin/bash
set -e

# ─────────────────────────────────────────────────────────────────────────────
# ⚠️ ملاحظة للمستقبل (README for Future Reference):
# 
# المشكلة: خطأ "relocation R_AARCH64... cannot be used against symbol"
# السبب: تعارض بين تعليمات الـ Assembly (NEON) الخاصة بـ FFmpeg 7.0 وبين 
#        قواعد الـ Relocation الصارمة في Android NDK r26c (أو أحدث).
# 
# الحل المتبع: استخدمنا "--disable-asm".
# 
# ما الذي يجب فعله للرجوع "للاستعمال الكامل" (استعادة الأداء):
# 1. إذا أردت استعادة سرعة الـ Assembly (NEON)، لا تحذف "--disable-asm" فحسب،
#    بل يجب التراجع إلى إصدار NDK أقدم (مثل r25b) حيث كانت قواعد الربط 
#    أقل صرامة، أو تحديث FFmpeg لإصدار أحدث إذا صدر إصدار يحل هذا التعارض.
# 2. إذا قمت بحذف "--disable-asm" مع بقاء NDK r26c، سيعود الخطأ للظهور،
#    وهذا يعني أنك بحاجة لإعادة ضبط أعلام الـ Linker أو تحديث بيئة البناء.
# ─────────────────────────────────────────────────────────────────────────────

# ─── 1. الإعدادات والمسارات ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg_source"
OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build"
OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"

# ─── 2. بناء Linux (يتم تنفيذه إذا كان TARGET_PLATFORM=linux) ────────────────
if [ "$TARGET_PLATFORM" = "linux" ]; then
    echo "⚙️  جاري بناء FFmpeg لـ Linux x86_64..."
    unset CC CXX AS
    mkdir -p "${OUTPUT_DIR}" "${FFMPEG_SRC_DIR}"
    
    if [ ! -f "${FFMPEG_SRC_DIR}/configure" ]; then
        wget -q --show-progress "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" -O "${SCRIPT_DIR}/ffmpeg.tar.gz"
        tar xzf "${SCRIPT_DIR}/ffmpeg.tar.gz" -C "${FFMPEG_SRC_DIR}" --strip-components=1
    fi
    
    cd "${FFMPEG_SRC_DIR}"
    make clean distclean 2>/dev/null || true
    ./configure --prefix="${OUTPUT_DIR}" --enable-static --disable-shared --disable-programs --disable-doc --enable-openssl --extra-cflags="-I${OPENSSL_BUILD}/include" --extra-ldflags="-L${OPENSSL_BUILD}/lib"
    make -j"$(nproc)" && make install
    echo "✅ تم بناء FFmpeg للينكس بنجاح!"
    exit 0
fi

# ─── 3. بناء Android ─────────────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
API_LEVEL="${API_LEVEL:-24}"

build_abi() {
    local ABI="$1" ARCH="$2" CPU="$3" CROSS_PREFIX_BIN="$4"
    local PREFIX="${OUTPUT_DIR}/${ABI}"
    mkdir -p "${PREFIX}"
    local TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
    
    export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang"
    export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang++"
    
    cd "${FFMPEG_SRC_DIR}"
    make clean distclean 2>/dev/null || true

    ./configure \
        --disable-asm \
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

if [ ! -d "${FFMPEG_SRC_DIR}/configure" ]; then
    mkdir -p "${FFMPEG_SRC_DIR}"
    wget -q --show-progress "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" -O ffmpeg.tar.gz
    tar xzf ffmpeg.tar.gz -C "${FFMPEG_SRC_DIR}" --strip-components=1
fi

build_abi "arm64-v8a" "aarch64" "armv8-a" "aarch64-linux-android"

echo "✅ تم البناء بنجاح!"
