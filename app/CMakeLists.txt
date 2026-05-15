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

add_library(NativeAppGlue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(NativeAppGlue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_subdirectory(bejzak_engine)

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

add_library(${PROJECT_NAME} SHARED main.cpp)

set(SHADER_COMPILE_SCRIPT
        "${PROJECT_SOURCE_DIR}/compile_shaders.sh")

file(CHMOD ${SHADER_COMPILE_SCRIPT} PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ)

add_custom_target(
        CompileShaders
        COMMAND ${SHADER_COMPILE_SCRIPT}
        COMMENT "Compiling shaders...")

add_dependencies(${PROJECT_NAME} CompileShaders)

target_compile_definitions(${PROJECT_NAME} PRIVATE XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_VULKAN
        VK_USE_PLATFORM_ANDROID_KHR)

target_link_libraries(${PROJECT_NAME}
        NativeAppGlue
        android
        BejzakEngine
        spdlog
        OpenXR::openxr_loader
)
