cmake_minimum_required(VERSION 3.22)
project(gdffmpeg VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── البنية المتوقعة للمجلد ───────────────────────────────────────────────────
#
#   gdffmpeg/                   (هذا الملف موجود هنا)
#   ├── CMakeLists.txt
#   ├── build_all.sh
#   ├── build_openssl_android.sh
#   ├── ffmpeg_build_android.sh
#   ├── gdffmpeg.gdextension
#   ├── src/
#   │   ├── ffmpeg_player.cpp
#   │   └── ffmpeg_player.h
#   ├── godot-cpp/
#   ├── openssl_build/          ← الناتج build_openssl_android.sh
#   │   ├── include/openssl/
#   │   └── lib/  (libssl.a + libcrypto.a)
#   └── ffmpeg_build/           ← ناتج ffmpeg_build_android.sh
#       └── arm64-v8a/
#           ├── include/
#           └── lib/
#
# ─────────────────────────────────────────────────────────────────────────────

set(GODOT_CPP_DIR  "${CMAKE_SOURCE_DIR}/godot-cpp"        CACHE PATH "مسار godot-cpp")
set(FFMPEG_DIR     "${CMAKE_SOURCE_DIR}/ffmpeg_build"     CACHE PATH "مسار FFmpeg الناتج")
set(OPENSSL_DIR    "${CMAKE_SOURCE_DIR}/openssl_build"    CACHE PATH "مسار OpenSSL الناتج")
set(ANDROID_ABI    "arm64-v8a"                            CACHE STRING "Android ABI")

set(FFMPEG_ABI_DIR "${FFMPEG_DIR}/${ANDROID_ABI}")

# ─── godot-cpp ────────────────────────────────────────────────────────────────
if(NOT TARGET godot-cpp)
    add_subdirectory(${GODOT_CPP_DIR} godot-cpp-build)
endif()

# ─── المصادر ─────────────────────────────────────────────────────────────────
set(SOURCES "${CMAKE_SOURCE_DIR}/src/ffmpeg_player.cpp")
set(HEADERS "${CMAKE_SOURCE_DIR}/src/ffmpeg_player.h")

# ─── المكتبة الديناميكية ─────────────────────────────────────────────────────
add_library(gdffmpeg SHARED ${SOURCES} ${HEADERS})

# ─── مسارات Include ──────────────────────────────────────────────────────────
target_include_directories(gdffmpeg PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${FFMPEG_ABI_DIR}/include"
    "${OPENSSL_DIR}/include"           # ترويسات OpenSSL (ssl.h, crypto.h...)
    "${GODOT_CPP_DIR}/include"
    "${GODOT_CPP_DIR}/gen/include"
)

# ─── ربط المكتبات ─────────────────────────────────────────────────────────────
target_link_libraries(gdffmpeg PRIVATE
    godot-cpp

    # FFmpeg (مرتبطة بـ OpenSSL)
    "${FFMPEG_ABI_DIR}/lib/libavformat.a"
    "${FFMPEG_ABI_DIR}/lib/libavcodec.a"
    "${FFMPEG_ABI_DIR}/lib/libswscale.a"
    "${FFMPEG_ABI_DIR}/lib/libswresample.a"
    "${FFMPEG_ABI_DIR}/lib/libavutil.a"

    # OpenSSL (للبروتوكولات المشفّرة HTTPS/TLS)
    "${OPENSSL_DIR}/lib/libssl.a"
    "${OPENSSL_DIR}/lib/libcrypto.a"

    # مكتبات نظام Android
    android       # واجهة النظام
    log           # Logcat
    z             # ضغط (zlib)
    m             # عمليات حسابية
    jnigraphics   # JNI / صور
    mediandk      # جسر MediaCodec العتادي
)

# ─── خيارات التجميع ───────────────────────────────────────────────────────────
target_compile_options(gdffmpeg PRIVATE
    -Wall
    -Wno-unused-parameter
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
    -Os                    # تحسين الحجم
)

target_link_options(gdffmpeg PRIVATE
    -Wl,--gc-sections      # حذف الكود غير المستخدم
    -Wl,--strip-all        # تقليص الحجم
)

# ─── اسم الملف الناتج ────────────────────────────────────────────────────────
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.android.arm64"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.android.arm32"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
endif()

# ─── رسالة ما بعد البناء ─────────────────────────────────────────────────────
add_custom_command(TARGET gdffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "✓ Build: $<TARGET_FILE:gdffmpeg>"
    COMMAND ${CMAKE_COMMAND} -E echo
        "  انسخ إلى: YOUR_GODOT_PROJECT/addons/gdffmpeg/bin/"
)
