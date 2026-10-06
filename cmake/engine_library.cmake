# FastPlay Engine: the engine as a shared library with a C interface
# (include/fastplay_engine/fastplay_engine.h), built instead of FastPlay when
# FASTPLAY_ENGINE_LIBRARY is on. It holds everything it needs (FFmpeg, the
# decoders, the tempo libraries, miniaudio) and exports only the fpe_ functions,
# so it sits beside an app's own copies of any of them.
#
# Windows: fastplay_engine.dll (+ .lib to link against). macOS: a universal
# libfastplay_engine.dylib, found through @rpath. iOS: FastPlayEngine.framework.

set(FPE_SOURCES
    src/engine_lib/api.cpp
    src/engine_lib/support.cpp
    src/audio/decoder_ffmpeg.cpp
    src/audio/decoder_midi.cpp
    src/audio/decoder_tracker.cpp
    src/audio/decoders.cpp
    src/audio/engine.cpp
    src/audio/http_cache.cpp
    src/audio/miniaudio.c
    src/audio/scrubber.cpp
    src/audio/tempo_processor.cpp
    src/http_common.cpp
    src/mp4_index.cpp
    src/mp4_remux.cpp
    src/utils.cpp
    src/xheaac.cpp
    src/youtube.cpp
    src/youtube_tools.cpp
    deps/speedy/speedy.c
    deps/speedy/soniclib.c
    deps/sonic/sonic.c
    deps/kissfft/kiss_fft.c
)
if(WIN32)
    list(APPEND FPE_SOURCES
        src/platform/http_windows.cpp
        src/platform/ini_windows.cpp
        src/platform/subprocess_windows.cpp
        src/platform/startup_windows.cpp
        src/platform/win7_compat.cpp
    )
elseif(FASTPLAY_IOS)
    list(APPEND FPE_SOURCES
        src/platform/http_ios.mm
        src/platform/ini_portable.cpp
        src/platform/subprocess_none.cpp
        src/platform/startup_ios.mm
    )
else()
    list(APPEND FPE_SOURCES
        src/platform/http_curl.cpp
        src/platform/ini_portable.cpp
        src/platform/subprocess_posix.cpp
    )
    if(APPLE)
        list(APPEND FPE_SOURCES src/platform/startup_mac.cpp)
    endif()
endif()

add_library(fastplay_engine SHARED ${FPE_SOURCES})

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(FDK_AAC_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/fdk-aac EXCLUDE_FROM_ALL)
target_compile_options(fdk-aac PRIVATE ${FASTPLAY_NO_WARNINGS})
# Static libraries going into a shared one
set_target_properties(fdk-aac spessasynth openmpt vorbis ogg PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(fastplay_engine PRIVATE fdk-aac spessasynth openmpt vorbis)

set_source_files_properties(deps/speedy/speedy.c deps/speedy/soniclib.c deps/sonic/sonic.c deps/kissfft/kiss_fft.c
    PROPERTIES COMPILE_OPTIONS ${FASTPLAY_NO_WARNINGS})
target_include_directories(fastplay_engine
    PUBLIC include
    PRIVATE
        src
        src/audio
        src/engine_lib
        include/fastplay
        ${FASTPLAY_FFMPEG_DIR}/include
        ${miniaudio_SOURCE_DIR}
        deps/speedy
        deps/sonic
        deps/kissfft
        deps/signalsmith-stretch
)
target_compile_definitions(fastplay_engine PRIVATE
    FPE_BUILDING
    FASTPLAY_ENGINE_LIBRARY
    USE_SPEEDY KISS_FFT SONIC_INTERNAL
    USE_SIGNALSMITH
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH
)
# Only the fpe_ functions are seen from outside
set_target_properties(fastplay_engine PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    OBJC_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# FFmpeg's static libraries were built with everything visible: on Apple, export
# only ours by name
if(APPLE)
    target_link_options(fastplay_engine PRIVATE "LINKER:-exported_symbol,_fpe_*")
endif()

if(WIN32)
    target_link_libraries(fastplay_engine PRIVATE
        ${FASTPLAY_FFMPEG_DIR}/lib/avformat.lib ${FASTPLAY_FFMPEG_DIR}/lib/avcodec.lib
        ${FASTPLAY_FFMPEG_DIR}/lib/swresample.lib ${FASTPLAY_FFMPEG_DIR}/lib/avutil.lib
        ws2_32 secur32 ncrypt crypt32 bcrypt
        user32 shell32 shlwapi advapi32 ole32 oleaut32 uuid winhttp)
    set_target_properties(fastplay_engine PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
    foreach(config Release Debug RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} upper)
        set_target_properties(fastplay_engine PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${upper} ${CMAKE_BINARY_DIR}/out
            ARCHIVE_OUTPUT_DIRECTORY_${upper} ${CMAKE_BINARY_DIR}/out)
    endforeach()
elseif(FASTPLAY_IOS)
    set_source_files_properties(src/audio/miniaudio.c PROPERTIES LANGUAGE OBJC)
    set_source_files_properties(src/platform/http_ios.mm src/platform/startup_ios.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    # FFmpeg for iOS comes as one set of libraries per SDK (ci/ffmpeg/build.sh ios)
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(FPE_FFMPEG_LIB ${FASTPLAY_FFMPEG_DIR}/lib/iphonesimulator)
    else()
        set(FPE_FFMPEG_LIB ${FASTPLAY_FFMPEG_DIR}/lib/iphoneos)
    endif()
    target_link_libraries(fastplay_engine PRIVATE
        ${FPE_FFMPEG_LIB}/libavformat.a ${FPE_FFMPEG_LIB}/libavcodec.a
        ${FPE_FFMPEG_LIB}/libswresample.a ${FPE_FFMPEG_LIB}/libavutil.a
        z "-framework Foundation" "-framework Security" "-framework CoreFoundation" "-framework CoreAudio"
        "-framework AudioToolbox" "-framework AVFoundation" "-framework UIKit")
    set_target_properties(fastplay_engine PROPERTIES
        FRAMEWORK TRUE
        OUTPUT_NAME FastPlayEngine
        MACOSX_FRAMEWORK_IDENTIFIER me.masonasons.fastplayengine
        MACOSX_FRAMEWORK_BUNDLE_VERSION 1
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING 1.0
        PUBLIC_HEADER include/fastplay_engine/fastplay_engine.h
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(fastplay_engine PRIVATE CURL::libcurl)
    if(APPLE)
        target_link_libraries(fastplay_engine PRIVATE
            ${FASTPLAY_FFMPEG_DIR}/lib/libavformat.a ${FASTPLAY_FFMPEG_DIR}/lib/libavcodec.a
            ${FASTPLAY_FFMPEG_DIR}/lib/libswresample.a ${FASTPLAY_FFMPEG_DIR}/lib/libavutil.a
            z "-framework Security" "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
        set_target_properties(fastplay_engine PROPERTIES
            INSTALL_NAME_DIR "@rpath"
            BUILD_WITH_INSTALL_NAME_DIR TRUE)
    else()
        target_link_libraries(fastplay_engine PRIVATE
            ${FASTPLAY_FFMPEG_DIR}/lib/libavformat.a ${FASTPLAY_FFMPEG_DIR}/lib/libavcodec.a
            ${FASTPLAY_FFMPEG_DIR}/lib/libswresample.a ${FASTPLAY_FFMPEG_DIR}/lib/libavutil.a
            z pthread dl m)
    endif()
    set_target_properties(fastplay_engine PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
endif()
