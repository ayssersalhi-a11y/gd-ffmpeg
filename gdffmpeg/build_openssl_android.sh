#!/bin/bash
# build_openssl_android.sh
# بناء OpenSSL 3.0 لنظام Android (arm64-v8a)
# ─────────────────────────────────────────────────────────────────────────────
# الناتج:
#   openssl_build/
#     ├── include/        (ملفات الترويسة .h)
#     └── lib/            (libssl.a + libcrypto.a)
# ─────────────────────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ─── إعدادات قابلة للتخصيص ────────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.0.13}"
OPENSSL_URL="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
API_LEVEL="${API_LEVEL:-24}"

SOURCE_DIR="${SCRIPT_DIR}/openssl_source"
OUTPUT_DIR="${SCRIPT_DIR}/openssl_build"

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║  بناء OpenSSL ${OPENSSL_VERSION} لـ Android arm64-v8a    ║"
echo "╚══════════════════════════════════════════════════╝"
echo "  NDK_PATH   : ${NDK_PATH}"
echo "  API Level  : ${API_LEVEL}"
echo "  OUTPUT_DIR : ${OUTPUT_DIR}"
echo ""

# ─── التحقق من NDK ────────────────────────────────────────────────────────────
if [ ! -d "${NDK_PATH}" ]; then
    echo "✗ لم يُعثر على NDK في: ${NDK_PATH}"
    echo "  عدّل متغير NDK_PATH أعلاه أو مرره عند الاستدعاء:"
    echo "  NDK_PATH=/path/to/ndk ./build_openssl_android.sh"
    exit 1
fi
echo "✓ NDK: ${NDK_PATH}"

# ─── التحقق من الأدوات المطلوبة ──────────────────────────────────────────────
for tool in wget tar make perl; do
    if ! command -v "$tool" &> /dev/null; then
        echo "✗ الأداة '$tool' غير مثبتة. ثبّتها أولاً."
        exit 1
    fi
done

# ─── تحميل كود OpenSSL ───────────────────────────────────────────────────────
mkdir -p "${SOURCE_DIR}"

TARBALL="${SCRIPT_DIR}/openssl-${OPENSSL_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ]; then
    echo "── تحميل OpenSSL ${OPENSSL_VERSION} ──"
    wget -q --show-progress "${OPENSSL_URL}" -O "${TARBALL}"
fi

if [ ! -d "${SOURCE_DIR}/Configure" ]; then
    echo "── استخراج الكود المصدري ──"
    tar xzf "${TARBALL}" -C "${SOURCE_DIR}" --strip-components=1
fi
echo "✓ كود OpenSSL جاهز في: ${SOURCE_DIR}"

# ─── إعداد متغيرات الـ Toolchain ─────────────────────────────────────────────
TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="${TOOLCHAIN}/sysroot"

export CC="${TOOLCHAIN}/bin/aarch64-linux-android${API_LEVEL}-clang"
export CXX="${TOOLCHAIN}/bin/aarch64-linux-android${API_LEVEL}-clang++"
export AR="${TOOLCHAIN}/bin/llvm-ar"
export LD="${TOOLCHAIN}/bin/ld"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
export NM="${TOOLCHAIN}/bin/llvm-nm"

# ─── التحقق من Clang ──────────────────────────────────────────────────────────
if [ ! -f "${CC}" ]; then
    echo "✗ لم يُعثر على Clang: ${CC}"
    echo "  تأكد من صحة NDK_PATH وAPI_LEVEL"
    exit 1
fi

# ─── بناء OpenSSL ─────────────────────────────────────────────────────────────
echo ""
echo "══ تهيئة OpenSSL لـ android-arm64 ══"
mkdir -p "${OUTPUT_DIR}"

cd "${SOURCE_DIR}"

# تنظيف أي بناء سابق
make clean 2>/dev/null || true

./Configure \
    android-arm64 \
    no-shared \
    no-tests \
    no-ui-console \
    no-stdio \
    --prefix="${OUTPUT_DIR}" \
    --openssldir="${OUTPUT_DIR}/ssl" \
    -D__ANDROID_API__=${API_LEVEL} \
    -fPIC \
    -fvisibility=hidden

echo "══ بناء OpenSSL (قد يستغرق 3-5 دقائق) ══"
make -j"$(nproc)" build_libs

echo "══ تثبيت الملفات ══"
make install_dev   # يثبّت include/ و lib/ فقط (بدون برامج)

cd "${SCRIPT_DIR}"

# ─── التحقق من النتائج ────────────────────────────────────────────────────────
echo ""
echo "══ فحص الملفات الناتجة ══"

REQUIRED_LIBS=("libssl.a" "libcrypto.a")
for lib in "${REQUIRED_LIBS[@]}"; do
    if [ -f "${OUTPUT_DIR}/lib/${lib}" ]; then
        SIZE=$(du -h "${OUTPUT_DIR}/lib/${lib}" | cut -f1)
        echo "  ✓ ${lib}  (${SIZE})"
    else
        echo "  ✗ ${lib} — مفقود! تحقق من سجلات البناء."
        exit 1
    fi
done

if [ -d "${OUTPUT_DIR}/include/openssl" ]; then
    COUNT=$(ls "${OUTPUT_DIR}/include/openssl/"*.h 2>/dev/null | wc -l)
    echo "  ✓ include/openssl/  (${COUNT} ملف .h)"
else
    echo "  ✗ مجلد include/openssl مفقود!"
    exit 1
fi

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║  ✓ OpenSSL جاهز بنجاح!                          ║"
echo "║  الناتج: openssl_build/include + openssl_build/lib ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "  الخطوة التالية: شغّل ./build_all.sh أو"
echo "  NDK_PATH=${NDK_PATH} ./ffmpeg_build_android.sh"
