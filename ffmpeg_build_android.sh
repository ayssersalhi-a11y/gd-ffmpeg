#!/bin/bash
set -e

# ─────────────────────────────────────────────────────────────────────────────
# ⚠️ ملاحظة للمستقبل (README for Future Reference):
# 
# المشكلة: خطأ "undefined symbol: inflateInit2_"
# السبب: عدم ربط مكتبة zlib أثناء عملية الـ Linking النهائية.
# 
# الحل المتبع: إضافة "--enable-zlib" وتضمين "-lz" في الروابط لضمان توفر
# خدمات ضغط البيانات التي يحتاجها libavformat.
# ─────────────────────────────────────────────────────────────────────────────
#
# ─────────────────────────────────────────────────────────────────────────────
# [v2 — إصلاحات شاملة]
# [FIX-1] أُضيف rtmp,rtsp لـ --enable-protocol (كان الكود C++ يدعم بث RTMP/RTSP
#         حيًا صراحة عبر reconnect_streamed دون أن يكون البروتوكول مبنيًا أصلاً).
# [FIX-2] أُضيفت h264_mediacodec/hevc_mediacodec/vp8_mediacodec/vp9_mediacodec
#         صراحة لـ --enable-decoder — بعد --disable-everything، تفعيل
#         --enable-mediacodec وحده يبني البنية التحتية فقط، لا أسماء الديكودر
#         الفعلية، فكان الكود يتراجع دائمًا وبصمت للـ Software Decode.
# [FIX-4] فُصل مخرج بناء Linux حسب المعمارية (linux-x86_64 / linux-arm64) بدل
#         مسار مسطّح مشترك — كان يُكتب الثاني فوق الأول عند البناء المحلي
#         المتتابع لمعماريتين مختلفتين (راجع CMakeLists.txt المُحدَّث ليطابق).
# [FIX-6] "if [ ! -d ... /configure ]" كان خطأً — configure ملف Perl/Shell
#         وليس مجلدًا، فكان الشرط صحيحًا دائمًا ويُعيد تحميل/استخراج الكود
#         المصدري لـ FFmpeg بالكامل في كل تشغيل محلي لبناء أندرويد.
# [FIX-7] أُضيف flv,rtsp لـ --enable-demuxer — بث RTMP يُغلَّف بصيغة FLV
#         تقريبًا دائمًا، وRTSP يحتاج اسم Demuxer منفصلاً عن اسم البروتوكول.
# [FIX-8] أُضيف vorbis لـ --enable-decoder — معظم ملفات .ogg الحقيقية هي
#         Vorbis لا Opus؛ روابط صوت خارجي شبكية بامتداد .ogg (عبر ext_fmt_ctx
#         في الكود C++) كانت ستفشل بفك التشفير رغم تفعيل Demuxer الحاوية.
# [FIX-11 — تم التراجع عنه، انظر [REVERT-FIX-11] أدناه] كانت هناك محاولة
#          لإزالة --disable-asm من بناء Linux x86_64 الأصلي فقط لتحسين
#          الأداء، لكن ثبت عمليًا (خطأ ربط فعلي على CI) أنها تكسر إنشاء
#          مكتبة مشتركة (.so) — ملفات Assembly يدوية غير آمنة الموقع رغم
#          --enable-pic. أُعيد --disable-asm للجميع؛ الاستقرار أهم من مكسب
#          أداء غير مضمون.
# ─────────────────────────────────────────────────────────────────────────────

# ─── 1. الإعدادات والمسارات ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg_source"
# [FIX-4] فصل مخرجات Linux حسب المعمارية (على غرار build_openssl_android.sh)
# بدل مسار مسطّح واحد كان يُكتب فوقه عند بناء x86_64 ثم arm64 بالتتابع محليًا.
if [ "$TARGET_PLATFORM" = "linux" ]; then
    OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build/linux-${TARGET_ARCH:-x86_64}"
else
    OUTPUT_DIR="${SCRIPT_DIR}/ffmpeg_build"
fi
if [ "$TARGET_PLATFORM" = "linux" ]; then
    OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build/linux-${TARGET_ARCH:-x86_64}"
else
    OPENSSL_BUILD="${SCRIPT_DIR}/openssl_build/android-arm64"
fi
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"

# ─── 2. بناء Linux (يتم تنفيذه إذا كان TARGET_PLATFORM=linux) ────────────────
if [ "$TARGET_PLATFORM" = "linux" ]; then
	
	rm -rf "${OUTPUT_DIR}/lib" "${OUTPUT_DIR}/include"
	unset CC CXX AS
	mkdir -p "${OUTPUT_DIR}" "${FFMPEG_SRC_DIR}"
	
	if [ ! -f "${FFMPEG_SRC_DIR}/configure" ]; then
		wget -q --show-progress "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" -O "${SCRIPT_DIR}/ffmpeg.tar.gz"
		tar xzf "${SCRIPT_DIR}/ffmpeg.tar.gz" -C "${FFMPEG_SRC_DIR}" --strip-components=1
	fi
	
	cd "${FFMPEG_SRC_DIR}"
	make clean distclean 2>/dev/null || true
	
	if [ "$TARGET_ARCH" = "arm64" ]; then
		echo "⚙️  جاري بناء FFmpeg لـ Linux ARM64 (aarch64) مع zlib و fPIC..."
		
		./configure \
			--prefix="${OUTPUT_DIR}" \
			--enable-static --disable-shared \
			--disable-programs --disable-doc \
			--disable-asm \
			--enable-pic \
			--enable-zlib \
			--enable-openssl \
			--arch=aarch64 \
			--target-os=linux \
			--enable-cross-compile \
			--cross-prefix=aarch64-linux-gnu- \
			--extra-cflags="-fPIC -I${OPENSSL_BUILD}/include" \
			--extra-ldflags="-L${OPENSSL_BUILD}/lib" \
			--extra-libs="-lz"
	else
		echo "⚙️  جاري بناء FFmpeg لـ Linux x86_64 مع zlib و fPIC..."

		# [REVERT-FIX-11 — v2] تراجعنا عن إزالة --disable-asm هنا. التجربة
		# الفعلية أثبتت أن --enable-pic وحده لا يضمن أن كل ملفات Assembly
		# اليدوية المحسَّنة (مثل vc1dsp_mmx.o) تُنتِج كودًا آمنًا للموقع
		# (Position-Independent) فعليًا — الخطأ الفعلي عند الربط النهائي:
		#   "relocation R_X86_64_PC32 against symbol `ff_pw_9' can not be
		#    used when making a shared object; recompile with -fPIC"
		# الاستقرار أهم من مكسب أداء غير مضمون هنا — نُبقي --disable-asm
		# للجميع (Android وكلا معماريتي Linux) كما كان الوضع الأصلي.
		./configure \
			--prefix="${OUTPUT_DIR}" \
			--enable-static --disable-shared \
			--disable-programs --disable-doc \
			--disable-asm \
			--enable-pic \
			--enable-zlib \
			--enable-openssl \
			--extra-cflags="-fPIC -I${OPENSSL_BUILD}/include" \
			--extra-ldflags="-L${OPENSSL_BUILD}/lib" \
			--extra-libs="-lz"
	fi
		
	make -j"$(nproc)" && make install
	echo "✅ تم بناء FFmpeg للينكس (مع zlib) بنجاح!"
	exit 0
fi


# ─── 3. بناء Android ─────────────────────────────────────────────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
API_LEVEL="${API_LEVEL:-24}"

build_abi() {
    local ABI="$1" ARCH="$2" CPU="$3" CROSS_PREFIX_BIN="$4"
    local PREFIX="${OUTPUT_DIR}/${ABI}"
    
    rm -rf "${PREFIX}"
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
        --enable-zlib \
        --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg4,aac,mp3,opus,flac,ac3,vorbis \
        --enable-decoder=h264_mediacodec,hevc_mediacodec,vp8_mediacodec,vp9_mediacodec \
        --enable-demuxer=mp4,matroska,mov,avi,hls,concat,mp3,ogg,aac,flac,wav,mpegts,flv,rtsp \
        --enable-parser=h264,hevc,aac,opus,mp3,flac,vorbis \
        --enable-protocol=file,pipe,http,https,hls,tcp,tls,ssl,crypto,data,ftp,rtmp,rtsp \
        --enable-openssl \
        --extra-cflags="-Os -fPIC -I${OPENSSL_BUILD}/include" \
        --extra-ldflags="-L${OPENSSL_BUILD}/lib" \
        --extra-libs="-lssl -lcrypto -lz"

    make -j"$(nproc)" && make install
}

if [ ! -f "${FFMPEG_SRC_DIR}/configure" ]; then
    mkdir -p "${FFMPEG_SRC_DIR}"
    wget -q --show-progress "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" -O ffmpeg.tar.gz
    tar xzf ffmpeg.tar.gz -C "${FFMPEG_SRC_DIR}" --strip-components=1
fi

build_abi "arm64-v8a" "aarch64" "armv8-a" "aarch64-linux-android"

echo "✅ تم البناء بنجاح!"
