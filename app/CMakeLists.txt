cmake_minimum_required(VERSION 3.22.1)
include(FetchContent)

project(AndroidAppBejzakEngine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")

add_definitions(-DSPDLOG_USE_STD_FORMAT)

add_compile_definitions(TEXTURES_PATH="textures/")
add_compile_definitions(MODELS_PATH="models/")

# add_library(OpenxrLoader SHARED IMPORTED GLOBAL)
# if (${CMAKE_BUILD_TYPE} STREQUAL Debug)
#     set_target_properties(OpenxrLoader PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/debug/arm64-v8a/libopenxr_loader.so")
# elseif (${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo) #android always generates debug symbols and the gradle plugin does the stripping part
#     set_target_properties(OpenxrLoader PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/release/arm64-v8a/libopenxr_loader.so")
# else ()
#     message(FATAL_ERROR "unknown build type")
# endif ()

add_library(NativeAppGlue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(NativeAppGlue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_subdirectory(bejzak_engine)
add_subdirectory(cpp)
