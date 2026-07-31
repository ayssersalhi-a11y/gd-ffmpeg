#!/bin/bash
# build_all.sh
# سكريبت رئيسي يشغّل مراحل البناء بالتتابع الصحيح:
#   المرحلة 1: بناء OpenSSL
#   المرحلة 2: بناء FFmpeg (مرتبط بـ OpenSSL)
#   المرحلة 3: بناء libgdffmpeg.so (مكتبة Godot)
# ─────────────────────────────────────────────────────────────────────────────
set -e

# 1. تحديد مسار السكريبت أولاً (ليعرف السكربت أين هو)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 2. الآن نضع خيار التنظيف الاختياري (الذي يعتمد على المجلدات في هذا المسار)
if [ "${CLEAN:-0}" = "1" ]; then
    echo -e "${YELLOW}🧹 خيار التنظيف مفعل: جارٍ حذف جميع مخلفات البناء...${NC}"
    # نستخدم SCRIPT_DIR لتحديد المسارات بدقة
    rm -rf "${SCRIPT_DIR}/ffmpeg_build" "${SCRIPT_DIR}/openssl_build" "${SCRIPT_DIR}/_cmake_build" "${SCRIPT_DIR}/dist"
    if [ -d "${SCRIPT_DIR}/ffmpeg_source" ]; then
        cd "${SCRIPT_DIR}/ffmpeg_source"
        make distclean 2>/dev/null || true
        cd ..
    fi
    echo -e "${GREEN}✓ تم التنظيف بنجاح.${NC}"
fi

# 3. ثم نكمل باقي الإعدادات...


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

OPENSSL_SUBDIR="android-arm64"
[ "${TARGET_PLATFORM}" = "linux" ] && OPENSSL_SUBDIR="linux-${TARGET_ARCH:-x86_64}"

if [ -f "${OPENSSL_BUILD}/${OPENSSL_SUBDIR}/lib/libssl.a" ] && [ -f "${OPENSSL_BUILD}/${OPENSSL_SUBDIR}/lib/libcrypto.a" ]; then
    log_ok "OpenSSL (${OPENSSL_SUBDIR}) موجود بالفعل — تجاوز البناء"
else
    chmod +x "${SCRIPT_DIR}/build_openssl_android.sh"
    TARGET_PLATFORM="${TARGET_PLATFORM}" TARGET_ARCH="${TARGET_ARCH}" "${SCRIPT_DIR}/build_openssl_android.sh"
fi

log_ok "OpenSSL جاهز: ${OPENSSL_BUILD}/${OPENSSL_SUBDIR}"

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

BUILD_TEMP="${SCRIPT_DIR}/_cmake_build"
DIST_DIR="${SCRIPT_DIR}/dist/addons/gdffmpeg/bin"
mkdir -p "${DIST_DIR}"

if [ "${TARGET_PLATFORM}" = "linux" ] && [ "${TARGET_ARCH}" = "arm64" ]; then
    # ── بناء Linux ARM64 (Cross-Compile) ──
    echo "  ── بناء Linux ARM64 (aarch64) ──"
    cmake \
        -S "${SCRIPT_DIR}" \
        -B "${BUILD_TEMP}/linux_arm64" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DGODOT_CPP_DIR="${GODOT_CPP_DIR}" \
        -DFFMPEG_DIR="${FFMPEG_BUILD}" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF

    cmake --build "${BUILD_TEMP}/linux_arm64" --parallel "$(nproc)"

    SO_LINUX_ARM64="${BUILD_TEMP}/linux_arm64/libgdffmpeg.linux.arm64.so"
    
    # البحث عن ملف الـ SO (اسم الملف قد يختلف قليلاً حسب إعدادات Sconstruct في godot-cpp)
    if [ ! -f "${SO_LINUX_ARM64}" ]; then
       SO_LINUX_ARM64=$(find "${BUILD_TEMP}/linux_arm64" -name "*.so" | head -n 1)
    fi

    if [ -n "${SO_LINUX_ARM64}" ] && [ -f "${SO_LINUX_ARM64}" ]; then
        cp "${SO_LINUX_ARM64}" "${DIST_DIR}/"
        log_ok "linux-arm64: $(du -h "${SO_LINUX_ARM64}" | cut -f1)"
    else
        log_err "لم يُعثر على مكتبة .so المبنية لـ Linux ARM64"
        exit 1
    fi

elif [ "${TARGET_PLATFORM}" = "linux" ] && [ "${TARGET_ARCH}" = "x86_64" ]; then
    # ── بناء Linux x86_64 (Native) ──
    echo "  ── بناء Linux x86_64 ──"
    cmake \
        -S "${SCRIPT_DIR}" \
        -B "${BUILD_TEMP}/linux_x86_64" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DGODOT_CPP_DIR="${GODOT_CPP_DIR}" \
        -DFFMPEG_DIR="${FFMPEG_BUILD}" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF

    cmake --build "${BUILD_TEMP}/linux_x86_64" --parallel "$(nproc)"
    
    SO_LINUX_X86=$(find "${BUILD_TEMP}/linux_x86_64" -name "*.so" | head -n 1)
    if [ -n "${SO_LINUX_X86}" ] && [ -f "${SO_LINUX_X86}" ]; then
        cp "${SO_LINUX_X86}" "${DIST_DIR}/"
        log_ok "linux-x86_64: $(du -h "${SO_LINUX_X86}" | cut -f1)"
    else
        log_err "لم يُعثر على مكتبة .so المبنية لـ Linux x86_64"
        exit 1
    fi

else
    # ── بناء Android (الافتراضي) ──
    echo "  ── بناء Android arm64-v8a ──"
    TOOLCHAIN_FILE="${NDK_PATH}/build/cmake/android.toolchain.cmake"
    
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
        -DCMAKE_VERBOSE_MAKEFILE=OFF

    cmake --build "${BUILD_TEMP}/arm64" --parallel "$(nproc)"

    SO_ARM64="${BUILD_TEMP}/arm64/libgdffmpeg.android.arm64.so"
    # احتياط: في حال كان الاسم مختلفاً
    if [ ! -f "${SO_ARM64}" ]; then
        SO_ARM64=$(find "${BUILD_TEMP}/arm64" -name "*.so" | head -n 1)
    fi

    if [ -n "${SO_ARM64}" ] && [ -f "${SO_ARM64}" ]; then
        cp "${SO_ARM64}" "${DIST_DIR}/"
        log_ok "android-arm64-v8a: $(du -h "${SO_ARM64}" | cut -f1)"
    else
        log_err "لم يُعثر على: ${SO_ARM64}"
        exit 1
    fi
fi

# نسخ ملف .gdextension
if [ -f "${SCRIPT_DIR}/gdffmpeg.gdextension" ]; then
    cp "${SCRIPT_DIR}/gdffmpeg.gdextension" "${SCRIPT_DIR}/dist/addons/gdffmpeg/"
    log_ok "gdffmpeg.gdextension ← مُنسَخ"
fi
