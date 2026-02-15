#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android_native_app_glue.h>

#include <spdlog/sinks/android_sink.h>
#include <spdlog/spdlog.h>

#include "openxr_wrapper/presentation/presentation.h"
#include "common/file/android_file_loader.h"

struct AndroidAppState {
  bool resumed = false;
};

static void AppHandleCmd(struct android_app *app, int32_t cmd) {
  auto *appState = reinterpret_cast<AndroidAppState *>(app->userData);
  switch (cmd) {
  case APP_CMD_START: {
    spdlog::info("APP_CMD_START onStart()");
    break;
  }
  case APP_CMD_RESUME: {
    spdlog::info("APP_CMD_RESUME onResume()");
    appState->resumed = true;
    break;
  }
  case APP_CMD_PAUSE: {
    spdlog::info("APP_CMD_PAUSE onPause()");
    appState->resumed = false;
    break;
  }
  case APP_CMD_STOP: {
    spdlog::info("APP_CMD_STOP onStop()");
    break;
  }
  case APP_CMD_DESTROY: {
    spdlog::info("APP_CMD_DESTROY onDestroy()");
    break;
  }
  case APP_CMD_INIT_WINDOW: {
    spdlog::info("APP_CMD_INIT_WINDOW surfaceCreated()");
    break;
  }
  case APP_CMD_TERM_WINDOW: {
    spdlog::info("APP_CMD_TERM_WINDOW surfaceDestroyed()");
    break;
  }
  }
}

struct PlatformData {
  void *application_vm;
  void *application_activity;
};

void android_main(struct android_app *app) {
  // sleep(10); // delay to allow debugger to attach
  try {
    auto android_logger =
        spdlog::android_logger_mt("android", "spdlog-android");
    android_logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(android_logger);
    JNIEnv *env;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AndroidAppState appState = {};

    app->userData = &appState;
    app->onAppCmd = AppHandleCmd;

    std::shared_ptr<PlatformData> data = std::make_shared<PlatformData>();
    data->application_vm = app->activity->vm;
    data->application_activity = app->activity->clazz;

    auto fileLoader = std::make_unique<AndroidFileLoader>(app->activity->assetManager);
    xrw::Presentation::create(xrw::AndroidAppState{
      .applicationVm = app->activity->vm,
      .applicationAcctivity = app->activity->clazz,
      .assetManager = app->activity->assetManager
    }, common::GraphicsApi::VULKAN, *fileLoader);
//    VrApp application;
//    application.init(app->activity->vm, app->activity->clazz,
//                     app->activity->assetManager);

    while (app->destroyRequested == 0) {
      for (;;) {
        spdlog::warn("BEJZAK");
        int events;
        struct android_poll_source *source;
//        const int timeout =
//            (!appState.resumed && !application.isSessionRunning() &&
//             app->destroyRequested == 0)
//                ? -1
//                : 0;
//        if (ALooper_pollOnce(timeout, nullptr, &events, (void **)&source) < 0) {
//          break;
//        }
        if (source != nullptr) {
          source->process(app, source);
        }
      }

//      application.pollEvents();
//      if (!application.isSessionRunning()) {
//        continue;
//      }
//
//      application.pollActions();
//      application.renderFrame();
    }

    app->activity->vm->DetachCurrentThread();
  } catch (const std::exception &ex) {
    spdlog::error(ex.what());
  } catch (...) {
    spdlog::error("Unknown Error");
  }
}
