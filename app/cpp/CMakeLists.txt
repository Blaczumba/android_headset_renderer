add_library(NativeAppGlue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(NativeAppGlue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

add_library(quest-xr SHARED main.cpp application.h)

set(SHADER_COMPILE_SCRIPT
        "${PROJECT_SOURCE_DIR}/cpp/compile_shaders.sh")

file(CHMOD ${SHADER_COMPILE_SCRIPT} PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ)

add_custom_target(
        CompileShaders
        COMMAND ${SHADER_COMPILE_SCRIPT}
        COMMENT "Compiling shaders...")

add_dependencies(quest-xr CompileShaders)

add_compile_definitions(TEXTURES_PATH="textures/")
add_compile_definitions(MODELS_PATH="models/")

target_compile_definitions(quest-xr PRIVATE XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_VULKAN
        VK_USE_PLATFORM_ANDROID_KHR)

target_link_libraries(
        quest-xr
        NativeAppGlue
        android
        meta_quest_openxr_loader
        BejzakEngine
        spdlog
)
