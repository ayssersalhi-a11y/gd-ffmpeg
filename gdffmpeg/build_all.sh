#!/bin/bash
# build_all.sh
# سكريبت رئيسي يُشغّل مراحل البناء بالتتابع الصحيح:
#   المرحلة 1: بناء OpenSSL
#   المرحلة 2: بناء FFmpeg (مرتبط بـ OpenSSL)
#   المرحلة 3: بناء libgdffmpeg.so (مكتبة Godot)
# ─────────────────────────────────────────────────────────────────────────────
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ─── إعدادات مركزية (عدّلها مرة واحدة هنا) ────────────────────────────────────
export NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
export FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"
export OPENSSL_VERSION="${OPENSSL_VERSION:-3.0.13}"
export API_LEVEL="${API_LEVEL:-24}"
GODOT_CPP_DIR="${GODOT_CPP_DIR:-${SCRIPT_DIR}/godot-cpp}"

# ─── ألوان الطرفية ────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # بدون لون

log_step() { echo -e "\n${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"; echo -e "${CYAN}  $1${NC}"; echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"; }
log_ok()   { echo -e "${GREEN}✓ $1${NC}"; }
log_warn() { echo -e "${YELLOW}⚠ $1${NC}"; }
log_err()  { echo -e "${RED}✗ $1${NC}"; }

# ─── رأس السكريبت ─────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║   بناء كامل: OpenSSL → FFmpeg → libgdffmpeg.so          ║"
echo "╠══════════════════════════════════════════════════════════╣"
echo "║  NDK_PATH      : ${NDK_PATH}"
echo "║  FFMPEG        : ${FFMPEG_VERSION}"
echo "║  OPENSSL       : ${OPENSSL_VERSION}"
echo "║  API Level     : ${API_LEVEL}"
echo "║  GODOT_CPP     : ${GODOT_CPP_DIR}"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

START_TIME=$SECONDS

# ─── التحقق من NDK ────────────────────────────────────────────────────────────
log_step "فحص المتطلبات"
if [ ! -d "${NDK_PATH}" ]; then
    log_err "لم يُعثر على NDK في: ${NDK_PATH}"
    echo "  حمّله من: https://developer.android.com/ndk/downloads"
    echo "  أو مرّر المسار: NDK_PATH=/path/to/ndk ./build_all.sh"
    exit 1
fi
log_ok "NDK موجود: ${NDK_PATH}"

for tool in wget tar make cmake ninja perl python3 git; do
    if command -v "$tool" &> /dev/null; then
        log_ok "$tool ✓"
    else
        log_warn "$tool غير موجود — قد يلزم تثبيته"
    fi
done

# ─── استنساخ godot-cpp إن لزم ─────────────────────────────────────────────────
log_step "المرحلة 0: التحقق من godot-cpp"
if [ ! -d "${GODOT_CPP_DIR}" ]; then
    echo "godot-cpp غير موجود. جارٍ الاستنساخ..."
    git clone --recursive \
        https://github.com/godotengine/godot-cpp.git \
        --branch godot-4.3-stable \
        --depth 1 \
        "${GODOT_CPP_DIR}"
fi
log_ok "godot-cpp: ${GODOT_CPP_DIR}"

# ─── المرحلة 1: بناء OpenSSL ─────────────────────────────────────────────────
log_step "المرحلة 1: بناء OpenSSL ${OPENSSL_VERSION}"

OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build"

if [ -f "${OPENSSL_BUILD}/lib/libssl.a" ] && [ -f "${OPENSSL_BUILD}/lib/libcrypto.a" ]; then
    log_ok "OpenSSL موجود بالفعل — تجاوز البناء (احذف openssl_build/ لإعادة البناء)"
else
    chmod +x "${SCRIPT_DIR}/build_openssl_android.sh"
    "${SCRIPT_DIR}/build_openssl_android.sh"
fi

log_ok "OpenSSL جاهز: ${OPENSSL_BUILD}"

# ─── المرحلة 2: بناء FFmpeg ────────────────────────────────────────────────────
log_step "المرحلة 2: بناء FFmpeg ${FFMPEG_VERSION} + OpenSSL"

FFMPEG_BUILD="${SCRIPT_DIR}/ffmpeg_build"

if [ -f "${FFMPEG_BUILD}/arm64-v8a/lib/libavcodec.a" ]; then
    log_ok "FFmpeg موجود بالفعل — تجاوز البناء (احذف ffmpeg_build/ لإعادة البناء)"
else
    chmod +x "${SCRIPT_DIR}/ffmpeg_build_android.sh"
    "${SCRIPT_DIR}/ffmpeg_build_android.sh"
fi

log_ok "FFmpeg جاهز: ${FFMPEG_BUILD}"

# ─── المرحلة 3: بناء libgdffmpeg.so ──────────────────────────────────────────
log_step "المرحلة 3: بناء libgdffmpeg.so (Godot GDExtension)"

TOOLCHAIN_FILE="${NDK_PATH}/build/cmake/android.toolchain.cmake"
BUILD_TEMP="${SCRIPT_DIR}/_cmake_build"
DIST_DIR="${SCRIPT_DIR}/dist/addons/gdffmpeg/bin"
mkdir -p "${DIST_DIR}"

# arm64-v8a
echo "  ── بناء arm64-v8a ──"
cmake \
    -S "${SCRIPT_DIR}" \
    -B "${BUILD_TEMP}/arm64" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-${API_LEVEL} \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DGODOT_CPP_DIR="${GODOT_CPP_DIR}" \
    -DFFMPEG_DIR="${FFMPEG_BUILD}" \
    -DOPENSSL_DIR="${OPENSSL_BUILD}" \
    -DCMAKE_VERBOSE_MAKEFILE=OFF

cmake --build "${BUILD_TEMP}/arm64" --parallel "$(nproc)"

# نسخ الملفات الناتجة
SO_ARM64="${BUILD_TEMP}/arm64/libgdffmpeg.android.arm64.so"
if [ -f "${SO_ARM64}" ]; then
    cp "${SO_ARM64}" "${DIST_DIR}/"
    log_ok "arm64-v8a: $(du -h "${DIST_DIR}/libgdffmpeg.android.arm64.so" | cut -f1)"
else
    log_err "لم يُعثر على: ${SO_ARM64}"
    exit 1
fi

# نسخ ملف .gdextension
if [ -f "${SCRIPT_DIR}/gdffmpeg.gdextension" ]; then
    cp "${SCRIPT_DIR}/gdffmpeg.gdextension" "${SCRIPT_DIR}/dist/addons/gdffmpeg/"
    log_ok "gdffmpeg.gdextension ← مُنسَخ"
fi

# ─── ملخص نهائي ───────────────────────────────────────────────────────────────
ELAPSED=$((SECONDS - START_TIME))
MINUTES=$((ELAPSED / 60))
SECS=$((ELAPSED % 60))

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ✓ البناء مكتمل بنجاح!                                  ║"
echo "╠══════════════════════════════════════════════════════════╣"
printf "║  الوقت المستغرق: %02d:%02d دقيقة\n" $MINUTES $SECS
echo "╠══════════════════════════════════════════════════════════╣"
echo "║  الملفات الجاهزة في: dist/addons/gdffmpeg/              ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

echo "  الملفات الناتجة:"
find "${SCRIPT_DIR}/dist" -type f | while read f; do
    SIZE=$(du -h "$f" | cut -f1)
    echo "    ${SIZE}  ${f#${SCRIPT_DIR}/}"
done

echo ""
echo "  الخطوة التالية:"
echo "  انسخ مجلد dist/addons/gdffmpeg/ إلى:"
echo "  YOUR_GODOT_PROJECT/addons/gdffmpeg/"
