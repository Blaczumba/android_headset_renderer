#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android_native_app_glue.h>

#include <spdlog/sinks/android_sink.h>
#include <spdlog/spdlog.h>

#include "openxr_wrapper/presentation/presentation.h"
#include "openxr_wrapper/platform/android_platform.h"
#include "common/file/android_file_loader.h"
#include "common/model_loader/tiny_gltf_loader/tiny_gltf_loader.h"

void android_main(struct android_app *app) {
  // sleep(10); // delay to allow debugger to attach
  try {
    auto android_logger =
        spdlog::android_logger_mt("android", "spdlog-android");
    android_logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(android_logger);

    auto platform = std::make_unique<xrw::AndroidPlatform>(app);
    auto fileLoader = std::make_unique<AndroidFileLoader>(app->activity->assetManager);
    setAssetmanager(app->activity->assetManager);
    auto presentation = xrw::Presentation::create(std::move(platform), common::GraphicsApi::VULKAN, *fileLoader);
    presentation->run();
  } catch (const std::exception &ex) {
    spdlog::error(ex.what());
  } catch (...) {
    spdlog::error("Unknown Error");
  }
}
