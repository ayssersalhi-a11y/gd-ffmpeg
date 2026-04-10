#!/bin/bash
# ffmpeg_build_android.sh  (v3.0 - OpenSSL Integration)
#
# التغييرات في v3.0:
#  - ربط OpenSSL المبني محلياً بدلاً من الاعتماد على النظام
#  - مسارات openssl_build/include و openssl_build/lib مباشرة
#  - تفعيل --enable-openssl مع مسارات صريحة (extra-cflags + extra-ldflags)
#  - الإبقاء على MediaCodec لضمان الأداء العتادي الكامل
#  - ناتج البناء في مجلد ffmpeg_build/
# ─────────────────────────────────────────────────────────────────────────────
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"
API_LEVEL="${API_LEVEL:-24}"

# مسار OpenSSL الذي تم بناؤه في المرحلة الأولى
OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build"

# مسار الكود المصدري لـ FFmpeg
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg_source"

# مسار ناتج البناء النهائي لـ FFmpeg
OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build"

echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║  بناء FFmpeg ${FFMPEG_VERSION} لـ Android (OpenSSL + MediaCodec)   ║"
echo "╚════════════════════════════════════════════════════════╝"
echo "  NDK_PATH      : ${NDK_PATH}"
echo "  OPENSSL_BUILD : ${OPENSSL_BUILD}"
echo "  OUTPUT_DIR    : ${OUTPUT_DIR}"
echo "  API Level     : ${API_LEVEL}"
echo ""

# ─── التحقق من المتطلبات ──────────────────────────────────────────────────────
if [ ! -d "${NDK_PATH}" ]; then
    echo "✗ لم يُعثر على NDK في: ${NDK_PATH}"
    exit 1
fi

if [ ! -f "${OPENSSL_BUILD}/lib/libssl.a" ]; then
    echo "✗ لم يُعثر على OpenSSL في: ${OPENSSL_BUILD}/lib/libssl.a"
    echo "  شغّل أولاً: ./build_openssl_android.sh"
    exit 1
fi

if [ ! -d "${OPENSSL_BUILD}/include/openssl" ]; then
    echo "✗ لم يُعثر على ترويسات OpenSSL في: ${OPENSSL_BUILD}/include/openssl"
    exit 1
fi
echo "✓ OpenSSL: موجود في ${OPENSSL_BUILD}"

# ─── تحميل FFmpeg إن لم يوجد ─────────────────────────────────────────────────
TARBALL="${SCRIPT_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz"
if [ ! -d "${FFMPEG_SRC_DIR}" ]; then
    mkdir -p "${FFMPEG_SRC_DIR}"
    if [ ! -f "${TARBALL}" ]; then
        echo "── تحميل FFmpeg ${FFMPEG_VERSION} ──"
        wget -q --show-progress \
            "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" \
            -O "${TARBALL}"
    fi
    echo "── استخراج FFmpeg ──"
    tar xzf "${TARBALL}" -C "${FFMPEG_SRC_DIR}" --strip-components=1
fi
echo "✓ FFmpeg: موجود في ${FFMPEG_SRC_DIR}"

# ─── دالة البناء ─────────────────────────────────────────────────────────────
build_abi() {
    local ABI="$1"
    local ARCH="$2"
    local CPU="$3"
    local CROSS_PREFIX_BIN="$4"

    local PREFIX="${OUTPUT_DIR}/${ABI}"
    mkdir -p "${PREFIX}"

    echo ""
    echo "══ بناء FFmpeg لـ ${ABI} ══"

    local TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
    local SYSROOT="${TOOLCHAIN}/sysroot"

    export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang"
    export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang++"
    export AR="${TOOLCHAIN}/bin/llvm-ar"
    export AS="${CC}"
    export NM="${TOOLCHAIN}/bin/llvm-nm"
    export STRIP="${TOOLCHAIN}/bin/llvm-strip"
    export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"

    # التحقق من وجود Clang للـ ABI المطلوب
    if [ ! -f "${CC}" ]; then
        echo "✗ لم يُعثر على: ${CC}"
        echo "  تحقق من NDK_PATH وAPI_LEVEL"
        exit 1
    fi

    cd "${FFMPEG_SRC_DIR}"

    # تنظيف بناء سابق
    make clean 2>/dev/null || true
    make distclean 2>/dev/null || true

    ./configure \
        --prefix="${PREFIX}" \
        --target-os=android \
        --arch="${ARCH}" \
        --cpu="${CPU}" \
        --enable-cross-compile \
        --sysroot="${SYSROOT}" \
        --cc="${CC}" \
        --cxx="${CXX}" \
        --ar="${AR}" \
        --nm="${NM}" \
        --ranlib="${RANLIB}" \
        --strip="${STRIP}" \
        \
        --enable-static \
        --disable-shared \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-everything \
        \
        --enable-avcodec \
        --enable-avformat \
        --enable-avutil \
        --enable-swscale \
        --enable-swresample \
        \
        --enable-jni \
        --enable-mediacodec \
        \
        --enable-decoder=h264_mediacodec \
        --enable-decoder=hevc_mediacodec \
        --enable-decoder=vp8_mediacodec \
        --enable-decoder=vp9_mediacodec \
        \
        --enable-decoder=h264 \
        --enable-decoder=hevc \
        --enable-decoder=vp8 \
        --enable-decoder=vp9 \
        --enable-decoder=av1 \
        --enable-decoder=mpeg4 \
        --enable-decoder=aac \
        --enable-decoder=mp3 \
        --enable-decoder=opus \
        --enable-decoder=flac \
        --enable-decoder=ac3 \
        \
        --enable-demuxer=mp4 \
        --enable-demuxer=matroska \
        --enable-demuxer=mov \
        --enable-demuxer=avi \
        --enable-demuxer=hls \
        --enable-demuxer=concat \
        \
        --enable-parser=h264 \
        --enable-parser=hevc \
        --enable-parser=aac \
        --enable-parser=opus \
        \
        --enable-protocol=file \
        --enable-protocol=pipe \
        --enable-protocol=http \
        --enable-protocol=https \
        --enable-protocol=hls \
        --enable-protocol=tcp \
        --enable-protocol=tls \
        --enable-protocol=crypto \
        \
        --enable-bsf=h264_mp4toannexb \
        --enable-bsf=hevc_mp4toannexb \
        --enable-bsf=aac_adtstoasc \
        \
        --enable-openssl \
        \
        --extra-cflags="-Os -fPIC -fvisibility=hidden -I${OPENSSL_BUILD}/include" \
        --extra-ldflags="-Wl,--gc-sections -L${OPENSSL_BUILD}/lib" \
        --extra-libs="-lssl -lcrypto -lz"

    make -j"$(nproc)"
    make install
    make clean

    cd "${SCRIPT_DIR}"

    echo "✓ ${ABI} → ${PREFIX}"

    echo "   المكتبات:"
    for lib in "${PREFIX}/lib/"*.a; do
        [ -f "$lib" ] && echo "     $(basename $lib)  ($(du -h "$lib" | cut -f1))"
    done
}

# ─── البناء ────────────────────────────────────────────────────────────────────
build_abi "arm64-v8a"   "aarch64" "armv8-a" "aarch64-linux-android"

# فعّل السطر التالي لبناء 32-bit أيضاً (اختياري)
# build_abi "armeabi-v7a" "arm"     "armv7-a" "armv7a-linux-androideabi"

# ─── ملخص نهائي ───────────────────────────────────────────────────────────────
echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║  ✓ FFmpeg + OpenSSL جاهزان!                           ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""
echo "  ✓ arm64-v8a: ${OUTPUT_DIR}/arm64-v8a/lib/"
ls -lh "${OUTPUT_DIR}/arm64-v8a/lib/" 2>/dev/null || true
echo ""
echo "  الخطوة التالية: شغّل ../build_gdffmpeg.sh لبناء libgdffmpeg.so"
