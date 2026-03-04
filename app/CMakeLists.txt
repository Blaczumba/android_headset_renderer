cmake_minimum_required(VERSION 3.22.1)
include(FetchContent)

project(AndroidAppBejzakEngine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")

add_subdirectory(meta_quest_openxr_loader)

add_definitions(-DSPDLOG_USE_STD_FORMAT)
FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.13.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(OpenXR-SDK
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.0.33 #must match the meta quest loader OpenXR version
        #https://developer.oculus.com/downloads/package/oculus-openxr-mobile-sdk#current-openxr-version
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(OpenXR-SDK)

add_compile_definitions(TEXTURES_PATH="textures/")
add_compile_definitions(MODELS_PATH="models/")

add_subdirectory(bejzak_engine)
add_subdirectory(cpp)
