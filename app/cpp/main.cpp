#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android_native_app_glue.h>

#include <spdlog/sinks/android_sink.h>
#include <spdlog/spdlog.h>

#include "application.h"
#include "openxr_wrapper/graphics_plugin/graphics_plugin_vulkan.h"
#include "openxr_wrapper/instance/instance.h"
#include "openxr_wrapper/platform/android_platform.h"
#include "openxr_wrapper/session/session.h"
#include "openxr_wrapper/space/space.h"
#include "openxr_wrapper/swapchain/swapchain.h"
#include "openxr_wrapper/system/system.h"
#include "openxr_wrapper/util/check.h"

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

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
              VkDebugUtilsMessageTypeFlagsEXT messageType,
              const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
              void *pUserData) {
  spdlog::warn("[Vulkan Validation] Severity: {}, Type: {}, Message: {}.",
               messageSeverity, messageType, pCallbackData->pMessage);
  return VK_FALSE;
}

class VrApp {
public:
  VrApp() = default;

  void init(void *applicationVm, void *applicationActivity,
            AAssetManager *assetManager) {
    xrw::AndroidData data = {applicationVm, applicationActivity};
    _platform = std::make_unique<xrw::AndroidPlatform>(data);
    _graphicsPlugin = std::make_unique<xrw::VulkanApplication>(
        debugCallback, assetManager,
        std::make_unique<AndroidFileLoader>(assetManager));

    _instance =
        xrw::Instance::create("BejzakEngine", *_platform, *_graphicsPlugin);
    _system = xrw::System::create(*_instance);
    _graphicsPlugin->initialize(_instance->getXrInstance(),
                                _system->getXrSystemId());
    _session = xrw::Session::create(*_system, *_graphicsPlugin);
    _swapchains = xrw::SwapchainBuilder()
                      .withArraySize(2)
                      .withViewConfigType(kConfigType)
                      .build(*_session, *_graphicsPlugin);
    _graphicsPlugin->createResources();
    _space = xrw::Space::create(_session->getXrSession(),
                                XR_REFERENCE_SPACE_TYPE_LOCAL);
  }

  void pollEvents() {
    while (const XrEventDataBaseHeader *event = tryReadNextEvent()) {
      switch (event->type) {
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
        const auto &instanceLossPending =
            reinterpret_cast<const XrEventDataInstanceLossPending &>(*event);
        spdlog::warn("XrEventDataInstanceLossPending by {}",
                     instanceLossPending.lossTime);
        return;
      }
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto &sessionStateChangedEvent =
            reinterpret_cast<const XrEventDataSessionStateChanged &>(*event);
        handleSessionStateChangedEvent(sessionStateChangedEvent);
        break;
      }
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
        //          XrSession xrSession = _session->getXrSession();
        //          LogActionSourceName(session_, input_.grab_action, "Grab");
        //          LogActionSourceName(session_, input_.quit_action, "Quit");
        //          LogActionSourceName(session_, input_.pose_action, "Pose");
        //          LogActionSourceName(session_, input_.vibrate_action,
        //          "Vibrate");
      } break;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      default: {
        spdlog::debug("Ignoring event type");
        break;
      }
      }
    }
  }

  void pollActions() {
    const XrActionStateGetInfo getInfo = {.type = XR_TYPE_ACTION_STATE_GET_INFO,
                                          .next = nullptr,
                                          .action = _input.quit_action,
                                          .subactionPath = XR_NULL_PATH};

    XrActionStateBoolean quitValue = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};

    // CHECK_XRCMD
    xrGetActionStateBoolean(_session->getXrSession(), &getInfo, &quitValue);
    if ((quitValue.isActive == XR_TRUE) &&
        (quitValue.changedSinceLastSync == XR_TRUE) &&
        (quitValue.currentState == XR_TRUE)) {
      // CHECK_XRCMD
      xrRequestExitSession(_session->getXrSession());
    }
  }

  const XrEventDataBaseHeader *tryReadNextEvent() {
    auto baseHeader =
        reinterpret_cast<XrEventDataBaseHeader *>(&_eventDataBuffer);
    baseHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
    XrResult result =
        xrPollEvent(_instance->getXrInstance(), &_eventDataBuffer);
    if (result == XR_SUCCESS) {
      if (baseHeader->type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
        auto eventsLost = reinterpret_cast<XrEventDataEventsLost *>(baseHeader);
        spdlog::warn("{} events lost", eventsLost->lostEventCount);
      }
      return baseHeader;
    }
    if (result != XR_EVENT_UNAVAILABLE) {
      spdlog::error("xr pull event unknown result");
    }
    return nullptr;
  }

  void handleSessionStateChangedEvent(
      const XrEventDataSessionStateChanged &stateChangedEvent) {
    if ((stateChangedEvent.session != XR_NULL_HANDLE) &&
        (stateChangedEvent.session != _session->getXrSession())) {
      spdlog::error("XrEventDataSessionStateChanged for unknown session");
      return;
    }
    _sessionState = stateChangedEvent.state;
    switch (_sessionState) {
    case XR_SESSION_STATE_READY: {
      const XrSessionBeginInfo sessionBeginInfo = {
          .type = XR_TYPE_SESSION_BEGIN_INFO,
          .primaryViewConfigurationType = kConfigType};

      xrBeginSession(_session->getXrSession(), &sessionBeginInfo);
      _sessionRunning = true;
      break;
    }
    case XR_SESSION_STATE_STOPPING: {
      _sessionRunning = false;
      xrEndSession(_session->getXrSession());
      break;
    }
    default:
      break;
    }
  }

  bool isSessionRunning() const { return _sessionRunning; }

  void renderFrame() {
    if (_session->getXrSession() == XR_NULL_HANDLE) {
      throw EngineException(
          "XrSession cannot be XR_NULL_HANDLE when rendering a frame.");
    }

    const XrFrameWaitInfo frameWaitInfo{
        .type = XR_TYPE_FRAME_WAIT_INFO,
    };

    XrFrameState frameState{
        .type = XR_TYPE_FRAME_STATE,
    };
    CHECK_XRCMD(
        xrWaitFrame(_session->getXrSession(), &frameWaitInfo, &frameState),
        "Failed to xrWaitFrame.");

    const XrFrameBeginInfo frameBeginInfo{
        .type = XR_TYPE_FRAME_BEGIN_INFO,
    };
    CHECK_XRCMD(xrBeginFrame(_session->getXrSession(), &frameBeginInfo),
                "Failed to xrBeginFrame.");

    std::vector<XrCompositionLayerBaseHeader *> layers{};
    XrCompositionLayerProjection layer{
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
    };
    std::vector<XrCompositionLayerProjectionView>
        projectionLayerViews; // It can be deleted
    if (frameState.shouldRender == XR_TRUE) {
      if (renderLayer(frameState.predictedDisplayTime, projectionLayerViews,
                      layer)) {
        layers.push_back(
            reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer));
      }
    }

    const XrFrameEndInfo frameEndInfo = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = frameState.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = static_cast<uint32_t>(layers.size()),
        .layers = layers.data()};

    CHECK_XRCMD(xrEndFrame(_session->getXrSession(), &frameEndInfo),
                "Failed to xrEndFrame.");
  }

  bool renderLayer(
      XrTime predictedDisplayTime,
      std::vector<XrCompositionLayerProjectionView> &projectionLayerViews,
      XrCompositionLayerProjection &layer) {
    XrViewState viewState = {.type = XR_TYPE_VIEW_STATE};

    const XrViewLocateInfo viewLocateInfo = {
        .type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = kConfigType,
        .displayTime = predictedDisplayTime,
        .space = _space->getXrSpace()};

    uint32_t viewCountOutput;
    // Not sure if it needs to remain (not local)
    lib::Buffer<XrView> views(_swapchains.size(), {.type = XR_TYPE_VIEW});
    CHECK_XRCMD(xrLocateViews(_session->getXrSession(), &viewLocateInfo,
                              &viewState, views.size(), &viewCountOutput,
                              views.data()),
                "Failed to xrLocateViews.");
    if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
      return false; // There is no valid tracking poses
                    // for the views.
    }

    // Render view to the appropriate part of the swapchain image.
    for (uint32_t i = 0; i < viewCountOutput; i++) {
      const xrw::Swapchain &viewSwapchain = _swapchains[i];

      const XrSwapchainImageAcquireInfo acquireInfo = {
          .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

      uint32_t swapchainImageIndex;
      CHECK_XRCMD(xrAcquireSwapchainImage(viewSwapchain.getSwapchain(),
                                          &acquireInfo, &swapchainImageIndex),
                  "Failed to xrAcquireSwapchainImage.");

      const XrSwapchainImageWaitInfo imageWaitInfo = {
          .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
          .timeout = XR_INFINITE_DURATION};

      CHECK_XRCMD(
          xrWaitSwapchainImage(viewSwapchain.getSwapchain(), &imageWaitInfo),
          "Failed to xrWaitSwapchainImage.");

      const XrCompositionLayerProjectionView &projectionLayerView =
          projectionLayerViews.emplace_back(XrCompositionLayerProjectionView{
              .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
              .pose = views[i].pose,
              .fov = views[i].fov,
              .subImage.swapchain = viewSwapchain.getSwapchain(),
              .subImage.imageRect.offset = {0, 0},
              .subImage.imageRect.extent = viewSwapchain.getXrExtent2Di()});

      _graphicsPlugin->draw(projectionLayerView, swapchainImageIndex);

      const XrSwapchainImageReleaseInfo releaseInfo = {
          .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      CHECK_XRCMD(
          xrReleaseSwapchainImage(viewSwapchain.getSwapchain(), &releaseInfo),
          "Failed to xrReleaseSwapchainImage.");
    }

    layer.space = _space->getXrSpace();
    layer.viewCount = static_cast<uint32_t>(projectionLayerViews.size());
    layer.views = projectionLayerViews.data();
    return true;
  }

  struct InputState {
    XrAction quit_action = XR_NULL_HANDLE;
  };

private:
  std::unique_ptr<xrw::Platform> _platform;
  std::unique_ptr<xrw::GraphicsPlugin> _graphicsPlugin;

  std::unique_ptr<xrw::Instance> _instance;
  std::unique_ptr<xrw::System> _system;
  std::unique_ptr<xrw::Session> _session;
  std::vector<xrw::Swapchain> _swapchains;
  std::unique_ptr<xrw::Space> _space;

  XrEventDataBuffer _eventDataBuffer;
  XrSessionState _sessionState; // MaybeLocal
  bool _sessionRunning = false;
  static constexpr inline XrViewConfigurationType kConfigType =
      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  InputState _input;
};

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

    VrApp application;
    application.init(app->activity->vm, app->activity->clazz,
                     app->activity->assetManager);

    while (app->destroyRequested == 0) {
      for (;;) {
        spdlog::warn("BEJZAK");
        int events;
        struct android_poll_source *source;
        const int timeout =
            (!appState.resumed && !application.isSessionRunning() &&
             app->destroyRequested == 0)
                ? -1
                : 0;
        if (ALooper_pollOnce(timeout, nullptr, &events, (void **)&source) < 0) {
          break;
        }
        if (source != nullptr) {
          source->process(app, source);
        }
      }

      application.pollEvents();
      if (!application.isSessionRunning()) {
        continue;
      }

      application.pollActions();
      application.renderFrame();
    }

    app->activity->vm->DetachCurrentThread();
  } catch (const std::exception &ex) {
    spdlog::error(ex.what());
  } catch (...) {
    spdlog::error("Unknown Error");
  }
}
