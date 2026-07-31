cmake_minimum_required(VERSION 3.22)
project(gdffmpeg VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── المسارات الأساسية ────────────────────────────────────────────────────────
set(GODOT_CPP_DIR  "${CMAKE_SOURCE_DIR}/godot-cpp" CACHE PATH "مسار godot-cpp")
set(FFMPEG_DIR     "${CMAKE_SOURCE_DIR}/ffmpeg_build" CACHE PATH "مسار FFmpeg")
set(OPENSSL_BASE_DIR "${CMAKE_SOURCE_DIR}/openssl_build" CACHE PATH "المسار الأساسي لـ OpenSSL")

if(ANDROID)
    set(OPENSSL_DIR "${OPENSSL_BASE_DIR}/android-arm64")
else()
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(OPENSSL_DIR "${OPENSSL_BASE_DIR}/linux-arm64")
    else()
        set(OPENSSL_DIR "${OPENSSL_BASE_DIR}/linux-x86_64")
    endif()
endif()

message(STATUS "OPENSSL_DIR resolved to: ${OPENSSL_DIR}")

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

# 1. تعريف مكتبات FFmpeg كـ Imported Target (هذا يضمن التعامل معها كمكتبات نظام)
set(FFMPEG_LIBS 
    avformat avcodec swscale swresample avutil
)

foreach(lib ${FFMPEG_LIBS})
    add_library(ffmpeg::${lib} STATIC IMPORTED)
    set_target_properties(ffmpeg::${lib} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_PATH}/lib${lib}.a"
        INTERFACE_POSITION_INDEPENDENT_CODE ON
    )
endforeach()

# 2. ربط المشروع
target_link_libraries(gdffmpeg PRIVATE
    godot-cpp
    ffmpeg::avformat
    ffmpeg::avcodec
    ffmpeg::swscale
    ffmpeg::swresample
    ffmpeg::avutil
    "${OPENSSL_DIR}/lib/libssl.a"
    "${OPENSSL_DIR}/lib/libcrypto.a"
)


# ─── ربط مكتبات النظام (ضروري جداً لتجنب أخطاء undefined symbol) ─────────────
if(IS_ANDROID)
    # للأندرويد: مكتبة z (zlib) مطلوبة بشدة، بالإضافة لمكتبات النظام الأساسية
    target_link_libraries(gdffmpeg PRIVATE
        z
        android
        log
        m
        jnigraphics
        mediandk
    )
else()
    # للينكس: pthread و dl و m و z ضرورية لكل من FFmpeg و OpenSSL
    target_link_libraries(gdffmpeg PRIVATE
        pthread
        dl
        m
        z
    )
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

# ─── اسم الملف الناتج (ذكي ومخصص حسب المنصة والمعمارية) ─────────────────────
if(IS_ANDROID)
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.android.arm64"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
else()
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set_target_properties(gdffmpeg PROPERTIES
            OUTPUT_NAME "gdffmpeg.linux.arm64"
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
endif()

# ─── رسالة ما بعد البناء ─────────────────────────────────────────────────────
add_custom_command(TARGET gdffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "✓ Build Success: $<TARGET_FILE:gdffmpeg>"
)
