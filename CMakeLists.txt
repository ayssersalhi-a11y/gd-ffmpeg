cmake_minimum_required(VERSION 3.22)
project(gdffmpeg VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── المسارات الأساسية ────────────────────────────────────────────────────────
set(GODOT_CPP_DIR  "${CMAKE_SOURCE_DIR}/godot-cpp" CACHE PATH "مسار godot-cpp")
set(FFMPEG_DIR     "${CMAKE_SOURCE_DIR}/ffmpeg_build" CACHE PATH "مسار FFmpeg")
set(OPENSSL_DIR    "${CMAKE_SOURCE_DIR}/openssl_build" CACHE PATH "مسار OpenSSL")

# ─── اكتشاف المنصة وإعداد المسارات الديناميكية ────────────────────────────────
if(ANDROID)
    set(IS_ANDROID ON)
    message(STATUS "Building for Android (ABI: ${ANDROID_ABI})")
    set(FFMPEG_LIB_PATH "${FFMPEG_DIR}/${ANDROID_ABI}/lib")
    set(FFMPEG_INC_PATH "${FFMPEG_DIR}/${ANDROID_ABI}/include")
else()
    set(IS_ANDROID OFF)
    message(STATUS "Building for Linux")
    set(FFMPEG_LIB_PATH "${FFMPEG_DIR}/lib")
    set(FFMPEG_INC_PATH "${FFMPEG_DIR}/include")
endif()

# ─── godot-cpp ────────────────────────────────────────────────────────────────
if(NOT TARGET godot-cpp)
    add_subdirectory(${GODOT_CPP_DIR} godot-cpp-build)
endif()

# ─── المصادر ─────────────────────────────────────────────────────────────────
set(SOURCES
    "${CMAKE_SOURCE_DIR}/src/ffmpeg_player.cpp"
    "${CMAKE_SOURCE_DIR}/src/ffmpeg_player.h"
)

# ─── المكتبة الديناميكية ─────────────────────────────────────────────────────
add_library(gdffmpeg SHARED ${SOURCES})

# ─── مسارات Include ──────────────────────────────────────────────────────────
target_include_directories(gdffmpeg PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${FFMPEG_INC_PATH}"
    "${OPENSSL_DIR}/include"
    "${GODOT_CPP_DIR}/include"
    "${GODOT_CPP_DIR}/gen/include"
)

# ─── ربط المكتبات ────────────────────────────────────────────────────────────
target_link_libraries(gdffmpeg PRIVATE
    godot-cpp
    "${FFMPEG_LIB_PATH}/libavformat.a"
    "${FFMPEG_LIB_PATH}/libavcodec.a"
    "${FFMPEG_LIB_PATH}/libswscale.a"
    "${FFMPEG_LIB_PATH}/libswresample.a"
    "${FFMPEG_LIB_PATH}/libavutil.a"
    "${OPENSSL_DIR}/lib/libssl.a"
    "${OPENSSL_DIR}/lib/libcrypto.a"
)

# ربط مكتبات النظام حسب نوع المنصة
if(IS_ANDROID)
    # المكتبات المطلوبة لدعم الوسائط والرسومات في أندرويد
    target_link_libraries(gdffmpeg PRIVATE android log z m jnigraphics mediandk)
else()
    # [FIX-5] إضافة z (zlib) و pthread و dl لأن FFmpeg+OpenSSL يحتاجانها على Linux
    target_link_libraries(gdffmpeg PRIVATE pthread dl m z)
endif()


# ─── خيارات التجميع ──────────────────────────────────────────────────────────
target_compile_options(gdffmpeg PRIVATE
    -Wall
    -Wno-unused-parameter
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
    -Os
)

target_link_options(gdffmpeg PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
)

# ─── اسم الملف الناتج (ذكي) ──────────────────────────────────────────────────
if(IS_ANDROID)
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.android.arm64"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
else()
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.linux.x86_64"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
endif()

# ─── رسالة ما بعد البناء ─────────────────────────────────────────────────────
add_custom_command(TARGET gdffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "✓ Build Success: $<TARGET_FILE:gdffmpeg>"
)
