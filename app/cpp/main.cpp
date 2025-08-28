#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android_native_app_glue.h>

#include "platform_data.hpp"
#include "platform.hpp"

#include "openxr_program.hpp"

#include <spdlog/sinks/android_sink.h>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include "openxr_wrapper/graphics_plugin/graphics_plugin_vulkan.h"
#include "application.h"
#include "openxr_wrapper/instance/instance.h"
#include "openxr_wrapper/system/system.h"
#include "openxr_wrapper/platform/android_platform.h"
#include "openxr_wrapper/session/session.h"
#include "openxr_wrapper/swapchain/swapchain.h"
#include "openxr_wrapper/space/space.h"
#include "openxr_wrapper/util/check.h"

struct AndroidAppState {
  bool resumed = false;
};

static void AppHandleCmd(struct android_app *app, int32_t cmd) {
  auto *app_state = reinterpret_cast<AndroidAppState *>(app->userData);
  switch (cmd) {
    case APP_CMD_START: {
      spdlog::info("APP_CMD_START onStart()");
      break;
    }
    case APP_CMD_RESUME: {
      spdlog::info("APP_CMD_RESUME onResume()");
      app_state->resumed = true;
      break;
    }
    case APP_CMD_PAUSE: {
      spdlog::info("APP_CMD_PAUSE onPause()");
      app_state->resumed = false;
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

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
  spdlog::warn("[Vulkan Validation] Severity: {}, Type: {}, Message: {}.",
               messageSeverity, messageType, pCallbackData->pMessage);
  return VK_FALSE;
}

class VrApp {
 public:
  VrApp() = default;

  Status init(void* applicationVm, void* applicationActivity, AAssetManager* assetManager) {
    xrw::AndroidData data = {applicationVm, applicationActivity};
    _platform = std::make_unique<xrw::AndroidPlatform>(data);
    _graphicsPlugin = std::make_unique<xrw::VulkanApplication>(debugCallback, assetManager);

    ASSIGN_OR_RETURN(_instance, xrw::Instance::create("BejzakEngine", *_platform, *_graphicsPlugin));
    ASSIGN_OR_RETURN(_system, xrw::System::create(*_instance));
    RETURN_IF_ERROR(_graphicsPlugin->initialize(_instance->getXrInstance(), _system->getXrSystemId()));
    ASSIGN_OR_RETURN(_session, xrw::Session::create(*_system, *_graphicsPlugin));
    ASSIGN_OR_RETURN(_swapchains, xrw::SwapchainBuilder()
        .withArraySize(2)
        .withViewConfigType(kConfigType)
        .build(*_session, *_graphicsPlugin));
    ASSIGN_OR_RETURN(_space,  xrw::Space::create(_session->getXrSession(),
                                                 XR_REFERENCE_SPACE_TYPE_LOCAL));
    return StatusOk();
  }

  void pollEvents() {
    while (const XrEventDataBaseHeader *event = tryReadNextEvent()) {
      switch (event->type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
          const auto &instance_loss_pending =
              *reinterpret_cast<const XrEventDataInstanceLossPending *>(&event);
          spdlog::warn("XrEventDataInstanceLossPending by {}", instance_loss_pending.lossTime);
          return;
        }
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
          auto session_state_changed_event =
              *reinterpret_cast<const XrEventDataSessionStateChanged *>(event);
          handleSessionStateChangedEvent(session_state_changed_event);
          break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
//          XrSession xrSession = _session->getXrSession();
//          LogActionSourceName(session_, input_.grab_action, "Grab");
//          LogActionSourceName(session_, input_.quit_action, "Quit");
//          LogActionSourceName(session_, input_.pose_action, "Pose");
//          LogActionSourceName(session_, input_.vibrate_action, "Vibrate");
        }
          break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        default: {
          spdlog::debug("Ignoring event type");
          break;
        }
      }
    }
  }

  Status pollActions() {
    XrActionStateGetInfo get_info{};
    get_info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    get_info.next = nullptr;
    get_info.action = _input.quit_action;
    get_info.subactionPath = XR_NULL_PATH;
    XrActionStateBoolean quit_value{};
    quit_value.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    CHECK_XRCMD(xrGetActionStateBoolean(_session->getXrSession(), &get_info, &quit_value));
    if ((quit_value.isActive == XR_TRUE) && (quit_value.changedSinceLastSync == XR_TRUE)
        && (quit_value.currentState == XR_TRUE)) {
      CHECK_XRCMD(xrRequestExitSession(_session->getXrSession()));
    }
    return StatusOk();
  }

  const XrEventDataBaseHeader* tryReadNextEvent() {
    auto baseHeader = reinterpret_cast<XrEventDataBaseHeader *>(&_eventDataBuffer);
    baseHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
    XrResult result = xrPollEvent(_instance->getXrInstance(), &_eventDataBuffer);
    if (result == XR_SUCCESS) {
      if (baseHeader->type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
        auto events_lost = reinterpret_cast<XrEventDataEventsLost *>(baseHeader);
        spdlog::warn("{} events lost", events_lost->lostEventCount);
      }
      return baseHeader;
    }
    if (result != XR_EVENT_UNAVAILABLE) {
      spdlog::error("xr pull event unknown result");
    }
    return nullptr;
  }

  Status handleSessionStateChangedEvent(const XrEventDataSessionStateChanged &stateChangedEvent) {
    if ((stateChangedEvent.session != XR_NULL_HANDLE)
        && (stateChangedEvent.session != _session->getXrSession())) {
      spdlog::error("XrEventDataSessionStateChanged for unknown session");
      return StatusOk();
    }
    _sessionState = stateChangedEvent.state;
    switch (_sessionState) {
      case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo session_begin_info{};
        session_begin_info.type = XR_TYPE_SESSION_BEGIN_INFO;
        session_begin_info.primaryViewConfigurationType = kConfigType;
        CHECK_XRCMD(xrBeginSession(_session->getXrSession(), &session_begin_info));
        _sessionRunning = true;
        break;
      }
      case XR_SESSION_STATE_STOPPING: {
        _sessionRunning = false;
        CHECK_XRCMD(xrEndSession(_session->getXrSession()));
        break;
      }
      default:break;
    }
    return StatusOk();
  }

  bool isSessionRunning() const {
    return _sessionRunning;
  }

  Status renderFrame() {
    if (_session->getXrSession() == XR_NULL_HANDLE) {
      throw std::runtime_error("session can not be null");
    }

    XrFrameWaitInfo frame_wait_info{
        .type = XR_TYPE_FRAME_WAIT_INFO,
    };
    XrFrameState frame_state{
        .type = XR_TYPE_FRAME_STATE,
    };
    CHECK_XRCMD(xrWaitFrame(_session->getXrSession(), &frame_wait_info, &frame_state));

    XrFrameBeginInfo frame_begin_info{
        .type = XR_TYPE_FRAME_BEGIN_INFO,
    };
    CHECK_XRCMD(xrBeginFrame(_session->getXrSession(), &frame_begin_info));

    std::vector<XrCompositionLayerBaseHeader *> layers{};
    XrCompositionLayerProjection layer{
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
    };
    std::vector<XrCompositionLayerProjectionView> projection_layer_views{};
    if (frame_state.shouldRender == XR_TRUE) {
       if (renderLayer(frame_state.predictedDisplayTime, projection_layer_views, layer)) {
         layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer));
       }
    }

    XrFrameEndInfo frame_end_info{};
    frame_end_info.type = XR_TYPE_FRAME_END_INFO;
    frame_end_info.displayTime = frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frame_end_info.layerCount = static_cast<uint32_t>(layers.size());
    frame_end_info.layers = layers.data();
    CHECK_XRCMD(xrEndFrame(_session->getXrSession(), &frame_end_info));
    return StatusOk();
  }

  Status renderLayer(XrTime predicted_display_time,
                                  std::vector<XrCompositionLayerProjectionView> &projection_layer_views,
                                  XrCompositionLayerProjection &layer) {
    XrViewState view_state{};
    view_state.type = XR_TYPE_VIEW_STATE;

    XrViewLocateInfo view_locate_info{};
    view_locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
    view_locate_info.viewConfigurationType = kConfigType;
    view_locate_info.displayTime = predicted_display_time;
    view_locate_info.space = _space->getXrSpace();

    uint32_t view_count_output = 0;
    // Not sure if it needs to remain (not local)
    lib::Buffer<XrView> views(_swapchains.size(), {.type = XR_TYPE_VIEW});
    CHECK_XRCMD(xrLocateViews(_session->getXrSession(),
                              &view_locate_info,
                              &view_state,
                              views.size(),
                              &view_count_output,
                              views.data()));
    if ((view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
      return Error(EngineError::NOT_FOUND);  // There is no valid tracking poses for the views.
    }

    projection_layer_views.resize(view_count_output);

    // For each locatable space that we want to visualize, render a 25cm cube.
//    std::vector<math::Transform> cubes{};
//
//    for (XrSpace visualized_space: visualized_spaces_) {
//      XrSpaceLocation space_location{};
//      space_location.type = XR_TYPE_SPACE_LOCATION;
//      auto res = xrLocateSpace(visualized_space, app_space_, predicted_display_time, &space_location);
//      if (XR_UNQUALIFIED_SUCCESS(res)) {
//        if ((space_location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
//            (space_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
//          cubes.push_back(math::Transform{
//              math::XrQuaternionFToGlm(space_location.pose.orientation),
//              math::XrVector3FToGlm(space_location.pose.position),
//              {0.25f, 0.25f, 0.25f}});
//        }
//      } else {
//        spdlog::debug("Unable to locate a visualized reference space in app space: {}",
//                      magic_enum::enum_name(res));
//      }
//    }

    // Render a 10cm cube scaled by grab_action for each hand. Note renderHand will only be true when the application has focus.
//    for (auto hand: {side::LEFT, side::RIGHT}) {
//      XrSpaceLocation space_location{};
//      space_location.type = XR_TYPE_SPACE_LOCATION;
//      auto res =
//          xrLocateSpace(input_.hand_space[hand], app_space_, predicted_display_time, &space_location);
//      if (XR_UNQUALIFIED_SUCCESS(res)) {
//        if ((space_location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
//            (space_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
//          float scale = 0.1f * input_.hand_scale[hand];
//          cubes.push_back(math::Transform{
//              math::XrQuaternionFToGlm(space_location.pose.orientation),
//              math::XrVector3FToGlm(space_location.pose.position),
//              {scale, scale, scale}});
//        }
//      } else {
//        // Tracking loss is expected when the hand is not active so only log a message if the hand is active.
//        if (input_.hand_active[hand] == XR_TRUE) {
//          const char *hand_name[] = {"left", "right"};
//          spdlog::debug("Unable to locate {} hand action space in app space: {}",
//                        hand_name[hand],
//                        magic_enum::enum_name(res));
//        }
//      }
//    }

    // Render view to the appropriate part of the swapchain image.
    for (uint32_t i = 0; i < view_count_output; i++) {
      const xrw::Swapchain& view_swapchain = _swapchains[i];

      XrSwapchainImageAcquireInfo acquire_info{};
      acquire_info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

      uint32_t swapchain_image_index = 0;
      CHECK_XRCMD(xrAcquireSwapchainImage(view_swapchain.getSwapchain(),
                                          &acquire_info,
                                          &swapchain_image_index));

      XrSwapchainImageWaitInfo wait_info{};
      wait_info.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
      wait_info.timeout = XR_INFINITE_DURATION;
      CHECK_XRCMD(xrWaitSwapchainImage(view_swapchain.getSwapchain(), &wait_info));

      projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
      projection_layer_views[i].pose = views[i].pose;
      projection_layer_views[i].fov = views[i].fov;
      projection_layer_views[i].subImage.swapchain = view_swapchain.getSwapchain();
      projection_layer_views[i].subImage.imageRect.offset = {0, 0};
      projection_layer_views[i].subImage.imageRect.extent = view_swapchain.getXrExtent2Di();

      ASSIGN_OR_RETURN(XrSwapchainImageBaseHeader* swapchain_image, _graphicsPlugin->getSwapchainImages(view_swapchain.getSwapchain()));
//      graphics_plugin_->RenderView(projection_layer_views[i],
//                                   swapchain_image,
//                                   swapchain_image_index,
//                                   cubes);

      XrSwapchainImageReleaseInfo release_info{};
      release_info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
      CHECK_XRCMD(xrReleaseSwapchainImage(view_swapchain.getSwapchain(), &release_info));
    }

    layer.space = _space->getXrSpace();
    layer.viewCount = static_cast<uint32_t>(projection_layer_views.size());
    layer.views = projection_layer_views.data();
    return StatusOk();
  }

  struct InputState {
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction grab_action = XR_NULL_HANDLE;
    XrAction pose_action = XR_NULL_HANDLE;
    XrAction vibrate_action = XR_NULL_HANDLE;
    XrAction quit_action = XR_NULL_HANDLE;
    std::array<XrPath, side::COUNT> hand_subaction_path{};
    std::array<XrSpace, side::COUNT> hand_space{};
    std::array<float, side::COUNT> hand_scale = {{1.0f, 1.0f}};
    std::array<XrBool32, side::COUNT> hand_active{};
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
  static constexpr inline XrViewConfigurationType kConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  InputState _input;
};

void android_main(struct android_app *app) {
  // sleep(10); // delay to allow debugger to attach
  try {
    auto android_logger = spdlog::android_logger_mt("android", "spdlog-android");
    android_logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(android_logger);

    JNIEnv *env;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AndroidAppState app_state = {};

    app->userData = &app_state;
    app->onAppCmd = AppHandleCmd;

    std::shared_ptr<PlatformData> data = std::make_shared<PlatformData>();
    data->application_vm = app->activity->vm;
    data->application_activity = app->activity->clazz;

    // std::shared_ptr<OpenXrProgram> program = CreateOpenXrProgram(CreatePlatform(data));

    //program->CreateInstance();
    //program->InitializeSystem();
    VrApp application;
    auto stat = application.init(app->activity->vm, app->activity->clazz, app->activity->assetManager);

    // program->InitializeSession();
    // program->CreateSwapchains();
    while (app->destroyRequested == 0) {
      for (;;) {
        spdlog::warn("BEJZAK");
        int events;
        struct android_poll_source *source;
        const int kTimeoutMilliseconds =
            (!app_state.resumed && !application.isSessionRunning() &&
                app->destroyRequested == 0) ? -1 : 0;
        if (ALooper_pollOnce(kTimeoutMilliseconds, nullptr, &events, (void **) &source) < 0) {
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
