#!/bin/bash
set -e

# ─── 1. الإعدادات الأساسية ───────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="${SCRIPT_DIR}/openssl_source"
if [ "$TARGET_PLATFORM" = "linux" ]; then
    OUTPUT_DIR="${SCRIPT_DIR}/openssl_build/linux-${TARGET_ARCH:-x86_64}"
else
    OUTPUT_DIR="${SCRIPT_DIR}/openssl_build/android-arm64"
fi
OPENSSL_VERSION="${OPENSSL_VERSION:-3.0.13}"
OPENSSL_URL="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"

echo "╔════════════════════════════════════════════════════════╗"
echo "║  بناء OpenSSL ${OPENSSL_VERSION} (يدعم Android و Linux)            ║"
echo "╚════════════════════════════════════════════════════════╝"

# ─── 2. التحقق من الأدوات المطلوبة ──────────────────────────────────────────
for tool in wget tar make perl; do
    if ! command -v "$tool" &> /dev/null; then
        echo "✗ الأداة '$tool' غير مثبتة. الرجاء تثبيتها أولاً."
        exit 1
    fi
done

# ─── 3. تحميل واستخراج الكود المصدري (للمنصتين) ─────────────────────────────
mkdir -p "${SOURCE_DIR}" "${OUTPUT_DIR}"
TARBALL="${SCRIPT_DIR}/openssl-${OPENSSL_VERSION}.tar.gz"

# [FIX-3] كان الشرط يتحقق من Configure كـ directory — والصحيح أنه ملف Perl script
if [ ! -f "${SOURCE_DIR}/Configure" ]; then
    if [ ! -f "${TARBALL}" ]; then
        echo "── تحميل OpenSSL ${OPENSSL_VERSION} ──"
        wget -q --show-progress "${OPENSSL_URL}" -O "${TARBALL}"
    fi
    echo "── استخراج الكود المصدري ──"
    tar xzf "${TARBALL}" -C "${SOURCE_DIR}" --strip-components=1
fi

echo "✓ كود OpenSSL جاهز في: ${SOURCE_DIR}"

cd "${SOURCE_DIR}"
make clean 2>/dev/null || true

# ─── 4. بناء اللينكس (إذا كان الهدف linux) ──────────────────────────────────
if [ "$TARGET_PLATFORM" = "linux" ]; then
	echo ""
	
	if [ "$TARGET_ARCH" = "arm64" ]; then
		echo "══ تهيئة OpenSSL لـ Linux ARM64 (aarch64) ══"
		
		# إعداد مترجم البناء المتقاطع
		export CROSS_COMPILE=aarch64-linux-gnu-
		
		./Configure linux-aarch64 no-shared -fPIC \
			--prefix="${OUTPUT_DIR}" \
			--openssldir="${OUTPUT_DIR}/ssl"
	else
		echo "══ تهيئة OpenSSL لـ Linux x86_64 ══"
		./Configure linux-x86_64 no-shared -fPIC \
			--prefix="${OUTPUT_DIR}" \
			--openssldir="${OUTPUT_DIR}/ssl"
	fi

	echo "══ بناء OpenSSL للينكس (قد يستغرق بعض الوقت) ══"
	make -j"$(nproc)"
	make install_dev

	# [FIX-6] على بعض توزيعات Linux يثبّت OpenSSL المكتبات في lib64 بدلاً من lib
	# نوحّد المسار بنسخ المحتوى إلى lib حتى يجده FFmpeg و CMake بشكل موثوق
	if [ -d "${OUTPUT_DIR}/lib64" ] && [ ! -f "${OUTPUT_DIR}/lib/libssl.a" ]; then
		echo "── lib64 مكتشف → نسخ المحتوى إلى lib ──"
		mkdir -p "${OUTPUT_DIR}/lib"
		cp -r "${OUTPUT_DIR}/lib64/." "${OUTPUT_DIR}/lib/"
		echo "  ✓ تم توحيد المسار: lib64 → lib"
	fi

# ─── 5. بناء الأندرويد (الافتراضي إذا لم يكن الهدف linux) ───────────────────
else
    NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
    API_LEVEL="${API_LEVEL:-24}"

    echo ""
    echo "══ تهيئة OpenSSL لـ Android arm64-v8a ══"
    echo "  NDK_PATH  : ${NDK_PATH}"
    echo "  API Level : ${API_LEVEL}"

    if [ ! -d "${NDK_PATH}" ]; then
        echo "✗ لم يُعثر على NDK في: ${NDK_PATH}"
        echo "  عدّل متغير NDK_PATH أو مرره عند الاستدعاء."
        exit 1
    fi
    echo "✓ NDK: ${NDK_PATH}"

    TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
    export PATH="${TOOLCHAIN}/bin:${PATH}"
    export ANDROID_NDK_ROOT="${NDK_PATH}"

    export CC="aarch64-linux-android${API_LEVEL}-clang"
    export CXX="aarch64-linux-android${API_LEVEL}-clang++"
    export AR="llvm-ar"
    export AS="llvm-as"
    export LD="ld"
    export RANLIB="llvm-ranlib"
    export STRIP="llvm-strip"
    export NM="llvm-nm"

    # التحقق من Clang (قبل البناء!)
    if ! command -v "${CC}" &> /dev/null; then
        echo "✗ لم يُعثر على المترجم في الـ PATH: ${CC}"
        if [ ! -f "${TOOLCHAIN}/bin/${CC}" ]; then
            echo "✗ خطأ فادح: المترجم غير موجود نهائياً."
            exit 1
        fi
    fi
    echo "✓ المترجم Clang جاهز."

    ./Configure \
        android-arm64 \
        no-shared \
        no-tests \
        no-ui-console \
        --prefix="${OUTPUT_DIR}" \
        --openssldir="${OUTPUT_DIR}/ssl" \
        -D__ANDROID_API__=${API_LEVEL} \
        -fPIC

    echo "══ بناء OpenSSL للأندرويد (قد يستغرق 3-5 دقائق) ══"
    make -j"$(nproc)" build_libs
    make install_dev
fi

# ─── 6. التحقق النهائي من النتائج (يعمل للمنصتين بنجاح) ──────────────────────
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
    COUNT=$(ls -1 "${OUTPUT_DIR}/include/openssl/"*.h 2>/dev/null | wc -l)
    echo "  ✓ include/openssl/  (${COUNT} ملف .h)"
else
    echo "  ✗ مجلد include/openssl مفقود!"
    exit 1
fi

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║  ✓ OpenSSL جاهز بنجاح!                           ║"
echo "║  الناتج: openssl_build/include + openssl_build/lib ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
