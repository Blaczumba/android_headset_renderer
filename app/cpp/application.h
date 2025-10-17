#pragma once

#include "graphics_plugin_vulkan.h"

#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <memory>
#include <queue>

#include "common/entity_component_system/component/material.h"
#include "common/entity_component_system/component/mesh.h"
#include "common/entity_component_system/component/transform.h"
#include "common/entity_component_system/registry/registry.h"
#include "common/file/android_file_loader.h"
#include "common/model_loader/model_loader.h"
#include "common/model_loader/obj_loader/obj_loader.h"
#include "common/model_loader/tiny_gltf_loader/tiny_gltf_loader.h"
#include "common/scene/octree.h"
#include "openxr_wrapper/util/check.h"
#include "vulkan_wrapper/descriptor_set/bindless_descriptor_set_writer.h"
#include "vulkan_wrapper/descriptor_set/descriptor_pool.h"
#include "vulkan_wrapper/descriptor_set/descriptor_set_writer.h"
#include "vulkan_wrapper/memory_objects/texture.h"
#include "vulkan_wrapper/pipeline/graphics_pipeline.h"
#include "vulkan_wrapper/pipeline/shader_program.h"
#include "vulkan_wrapper/render_pass/render_pass.h"
#include "vulkan_wrapper/resource_manager/asset_manager.h"
#include "vulkan_wrapper/util/check.h"

namespace {

lib::Buffer<VkBufferImageCopy>
createBufferImageCopyRegions(std::span<const ImageSubresource> subresources) {
  lib::Buffer<VkBufferImageCopy> regions(subresources.size());
  std::transform(
      std::cbegin(subresources), std::cend(subresources), regions.begin(),
      [](const ImageSubresource &subresource) {
        return VkBufferImageCopy{
            .bufferOffset = subresource.offset,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = subresource.mipLevel,
                                 .baseArrayLayer = subresource.baseArrayLayer,
                                 .layerCount = subresource.layerCount},
            .imageExtent = {.width = subresource.width,
                            .height = subresource.height,
                            .depth = subresource.depth}};
      });
  return regions;
}

ErrorOr<Texture> createCubemap(const LogicalDevice &logicalDevice,
                               VkCommandBuffer commandBuffer,
                               const AssetManager::ImageData &imageData,
                               VkFormat format, float samplerAnisotropy) {
  return TextureBuilder()
      .withAspect(VK_IMAGE_ASPECT_COLOR_BIT)
      .withExtent(imageData.width, imageData.height)
      .withFormat(format)
      .withMipLevels(imageData.mipLevels)
      .withUsage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
      .withLayerCount(6)
      .withMaxAnisotropy(samplerAnisotropy)
      .withMaxLod(static_cast<float>(imageData.mipLevels))
      .withLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      .buildImage(logicalDevice, commandBuffer,
                  imageData.stagingBuffer.getVkBuffer(),
                  createBufferImageCopyRegions(imageData.copyRegions));
}

ErrorOr<Texture> createShadowmap(const LogicalDevice &logicalDevice,
                                 VkCommandBuffer commandBuffer, uint32_t width,
                                 uint32_t height, VkFormat format) {
  return TextureBuilder()
      .withAspect(VK_IMAGE_ASPECT_DEPTH_BIT)
      .withExtent(width, height)
      .withFormat(format)
      .withUsage(VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      .withAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)
      .withCompareOp(VK_COMPARE_OP_LESS_OR_EQUAL)
      .withBorderColor(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
      .buildImageSampler(logicalDevice, commandBuffer);
}

ErrorOr<Texture> createTexture2D(const LogicalDevice &logicalDevice,
                                 VkCommandBuffer commandBuffer,
                                 const AssetManager::ImageData &imageData,
                                 VkFormat format, float samplerAnisotropy) {
  return TextureBuilder()
      .withAspect(VK_IMAGE_ASPECT_COLOR_BIT)
      .withExtent(imageData.width, imageData.height)
      .withFormat(format)
      .withMipLevels(imageData.mipLevels)
      .withUsage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
      .withMaxAnisotropy(samplerAnisotropy)
      .withMaxLod(static_cast<float>(imageData.mipLevels))
      .buildMipmapImage(logicalDevice, commandBuffer,
                        imageData.stagingBuffer.getVkBuffer(),
                        createBufferImageCopyRegions(imageData.copyRegions));
}

} // namespace

namespace xrw {

class VulkanApplication : public GraphicsPluginVulkan {
public:
  VulkanApplication(PFN_vkDebugUtilsMessengerCallbackEXT debugCallback,
                    AAssetManager *assetManager,
                    const std::shared_ptr<FileLoader> &fileLoader)
      : GraphicsPluginVulkan(debugCallback),
        _assetManager(_logicalDevice, fileLoader, std::launch::deferred),
        _programManager(fileLoader), _fileLoader(fileLoader) {
    setAssetmanager(assetManager);
  }

  Status createResources() override {
    RETURN_IF_ERROR(loadCubemap());
    RETURN_IF_ERROR(createDescriptorSets());
    RETURN_IF_ERROR(createPresentResources());
    RETURN_IF_ERROR(createShadowResources());
    RETURN_IF_ERROR(createCommandBuffers());
    RETURN_IF_ERROR(createSyncObjects());
    RETURN_IF_ERROR(loadObjects());
    RETURN_IF_ERROR(createOctreeScene());
    {
      SingleTimeCommandBuffer handle(*_singleTimeCommandPool);
      recordShadowCommandBuffer(handle.getCommandBuffer(), 0);
    }
    return StatusOk();
  }

private:
  AssetManager _assetManager;
  ShaderProgramManager _programManager;
  std::shared_ptr<FileLoader> _fileLoader;

  Buffer _vertexBufferCube;
  Buffer _indexBufferCube;
  VkIndexType _indexBufferCubeType;
  Texture _textureCubemap;
  ShaderProgram _skyboxShaderProgram;
  TextureHandle _skyboxHandle;

  DescriptorSetWriter _dynamicDescriptorSetWriter;
  Buffer _dynamicUniformBuffersCamera;
  DescriptorSet _dynamicDescriptorSet;

  // gltf objects
  std::unordered_map<std::string, std::pair<TextureHandle, Texture>> _textures;
  std::vector<Object> _objects;
  std::unique_ptr<Octree> _octree;
  Registry _registry;
  ShaderProgram _pbrShaderProgram;
  std::vector<Framebuffer> _framebuffers;
  std::vector<Texture> _attachments;
  std::unique_ptr<GraphicsPipeline>
      _graphicsPipeline; // Does not have to be unique_ptr

  // shadow map
  ShaderProgram _shadowShaderProgram;
  Renderpass _shadowRenderPass;
  Framebuffer _shadowFramebuffer;
  std::unique_ptr<GraphicsPipeline>
      _shadowPipeline; // Does not have to be unique_ptr
  Texture _shadowMap;
  TextureHandle _shadowHandle;

  UniformBufferLight _ubLight;

  Buffer _lightBuffer;
  BufferHandle _lightHandle;

  std::shared_ptr<DescriptorPool> _descriptorPool;
  std::shared_ptr<DescriptorPool> _dynamicDescriptorPool;
  DescriptorSet _bindlessDescriptorSet;
  std::unique_ptr<BindlessDescriptorSetWriter>
      _bindlessWriter; // Does not have to be unique_ptr

  Renderpass _renderpass;

  std::unique_ptr<GraphicsPipeline> _graphicsPipelineSkybox;

  uint8_t _currentFrame = 0;

  Status loadCubemap() {
    _assetManager.loadImageAsync(TEXTURES_PATH "cubemap_yokohama_rgba.ktx");
    ASSIGN_OR_RETURN(std::string data,
                     _fileLoader->loadFileToString(MODELS_PATH "cube.obj"));
    ASSIGN_OR_RETURN(VertexData vertexDataCube,
                     loadObj(_assetManager, "cube.obj", data));

    {
      SingleTimeCommandBuffer handle(*_singleTimeCommandPool);
      const VkCommandBuffer commandBuffer = handle.getCommandBuffer();

      // Load texture.
      ASSIGN_OR_RETURN(const AssetManager::ImageData &imageData,
                       _assetManager.getImageData(TEXTURES_PATH
                                                  "cubemap_yokohama_rgba.ktx"));
      ASSIGN_OR_RETURN(
          _textureCubemap,
          createCubemap(_logicalDevice, commandBuffer, imageData,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        _physicalDevice->getMaxSamplerAnisotropy()));

      // Load geometry.
      ASSIGN_OR_RETURN(const AssetManager::VertexData &vData,
                       _assetManager.getVertexData("cube.obj"));
      ASSIGN_OR_RETURN(
          _vertexBufferCube,
          Buffer::createVertexBuffer(_logicalDevice,
                                     vData.vertexBufferPositions.getSize()));
      RETURN_IF_ERROR(_vertexBufferCube.copyBuffer(
          commandBuffer, vData.vertexBufferPositions));
      ASSIGN_OR_RETURN(_indexBufferCube,
                       Buffer::createIndexBuffer(_logicalDevice,
                                                 vData.indexBuffer.getSize()));
      RETURN_IF_ERROR(
          _indexBufferCube.copyBuffer(commandBuffer, vData.indexBuffer));
      _indexBufferCubeType = vData.indexType;
    }

    return StatusOk();
  }

  Status loadObjects() {
    // TODO needs refactoring
    ASSIGN_OR_RETURN(
        const std::vector<VertexData> sceneData,
        LoadGltfFromFile(_assetManager, MODELS_PATH "sponza/scene.gltf"));
    const float maxSamplerAnisotropy =
        _physicalDevice->getMaxSamplerAnisotropy();
    _objects.reserve(sceneData.size());

    SingleTimeCommandBuffer handle(*_singleTimeCommandPool);
    const VkCommandBuffer commandBuffer = handle.getCommandBuffer();
    for (const VertexData &sceneObject : sceneData) {
      const std::string diffusePath =
          MODELS_PATH "sponza/" + sceneObject.diffuseTexture;
      if (!_textures.contains(diffusePath)) {
        ASSIGN_OR_RETURN(const AssetManager::ImageData &imgData,
                         _assetManager.getImageData(diffusePath));
        ASSIGN_OR_RETURN(Texture texture,
                         createTexture2D(_logicalDevice, commandBuffer, imgData,
                                         VK_FORMAT_R8G8B8A8_SRGB,
                                         maxSamplerAnisotropy));
        _textures.emplace(diffusePath,
                          std::make_pair(_bindlessWriter->storeTexture(texture),
                                         std::move(texture)));
      }
      const std::string normalPath =
          MODELS_PATH "sponza/" + sceneObject.normalTexture;
      if (!_textures.contains(normalPath)) {
        ASSIGN_OR_RETURN(const AssetManager::ImageData &imgData,
                         _assetManager.getImageData(normalPath));
        ASSIGN_OR_RETURN(Texture texture,
                         createTexture2D(_logicalDevice, commandBuffer, imgData,
                                         VK_FORMAT_R8G8B8A8_UNORM,
                                         maxSamplerAnisotropy));
        _textures.emplace(normalPath,
                          std::make_pair(_bindlessWriter->storeTexture(texture),
                                         std::move(texture)));
      }
      const std::string metallicRoughnessPath =
          MODELS_PATH "sponza/" + sceneObject.metallicRoughnessTexture;
      if (!_textures.contains(metallicRoughnessPath)) {
        ASSIGN_OR_RETURN(const AssetManager::ImageData &imgData,
                         _assetManager.getImageData(metallicRoughnessPath));
        ASSIGN_OR_RETURN(Texture texture,
                         createTexture2D(_logicalDevice, commandBuffer, imgData,
                                         VK_FORMAT_R8G8B8A8_UNORM,
                                         maxSamplerAnisotropy));
        _textures.emplace(metallicRoughnessPath,
                          std::make_pair(_bindlessWriter->storeTexture(texture),
                                         std::move(texture)));
      }
      Entity e = _registry.createEntity();
      _objects.emplace_back("", e);
      _registry.addComponent<MaterialComponent>(
          e, MaterialComponent{_textures[diffusePath].first,
                               _textures[normalPath].first,
                               _textures[metallicRoughnessPath].first});
      ASSIGN_OR_RETURN(const AssetManager::VertexData &vData,
                       _assetManager.getVertexData(sceneObject.vertexResource));
      MeshComponent msh;
      ASSIGN_OR_RETURN(msh.vertexBuffer,
                       Buffer::createVertexBuffer(
                           _logicalDevice, vData.vertexBuffer.getSize()));
      RETURN_IF_ERROR(
          msh.vertexBuffer.copyBuffer(commandBuffer, vData.vertexBuffer));
      ASSIGN_OR_RETURN(msh.indexBuffer,
                       Buffer::createIndexBuffer(_logicalDevice,
                                                 vData.indexBuffer.getSize()));
      RETURN_IF_ERROR(
          msh.indexBuffer.copyBuffer(commandBuffer, vData.indexBuffer));
      ASSIGN_OR_RETURN(
          msh.vertexBufferPrimitive,
          Buffer::createVertexBuffer(_logicalDevice,
                                     vData.vertexBufferPositions.getSize()));
      RETURN_IF_ERROR(msh.vertexBufferPrimitive.copyBuffer(
          commandBuffer, vData.vertexBufferPositions));
      msh.indexType = vData.indexType;
      msh.aabb =
          createAABBfromVertices(sceneObject.positions, sceneObject.model);
      _registry.addComponent<MeshComponent>(e, std::move(msh));

      TransformComponent trsf;
      trsf.model = sceneObject.model;
      _registry.addComponent<TransformComponent>(e, std::move(trsf));
    }

    return StatusOk();
  }

  Status createOctreeScene() {
    AABB sceneAABB =
        _registry.getComponent<MeshComponent>(_objects[0].getEntity()).aabb;

    for (int i = 1; i < _objects.size(); ++i) {
      sceneAABB.extend(
          _registry.getComponent<MeshComponent>(_objects[i].getEntity()).aabb);
    }
    _octree = std::make_unique<Octree>(sceneAABB);

    for (const Object &object : _objects)
      _octree->addObject(
          &object,
          _registry.getComponent<MeshComponent>(object.getEntity()).aabb);

    return StatusOk();
  }

  Status createDescriptorSets() {
    ASSIGN_OR_RETURN(_pbrShaderProgram,
                     _programManager.createPBRProgram(_logicalDevice));
    ASSIGN_OR_RETURN(_shadowShaderProgram,
                     _programManager.createShadowProgram(_logicalDevice));
    ASSIGN_OR_RETURN(_skyboxShaderProgram,
                     _programManager.createSkyboxProgram(_logicalDevice));

    ASSIGN_OR_RETURN(_descriptorPool,
                     DescriptorPool::create(
                         _logicalDevice, 150,
                         VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT));
    ASSIGN_OR_RETURN(_bindlessDescriptorSet,
                     _descriptorPool->createDesriptorSet(
                         _programManager.getVkDescriptorSetLayout(
                             DescriptorSetType::BINDLESS)));
    _bindlessWriter =
        std::make_unique<BindlessDescriptorSetWriter>(_bindlessDescriptorSet);
    _skyboxHandle = _bindlessWriter->storeTexture(_textureCubemap);

    {
      // TODO: Should not be in this function.
      SingleTimeCommandBuffer handle(*_singleTimeCommandPool);
      const VkCommandBuffer commandBuffer = handle.getCommandBuffer();
      ASSIGN_OR_RETURN(_shadowMap,
                       createShadowmap(_logicalDevice, commandBuffer, 1024 * 2,
                                       1024 * 2, VK_FORMAT_D32_SFLOAT));
    }

    _shadowHandle = _bindlessWriter->storeTexture(_shadowMap);

    const uint32_t size =
        _physicalDevice->getMemoryAlignment(sizeof(UniformBufferCamera));
    ASSIGN_OR_RETURN(_dynamicUniformBuffersCamera,
                     Buffer::createUniformBuffer(_logicalDevice,
                                                 MAX_FRAMES_IN_FLIGHT * size));
    _dynamicDescriptorSetWriter.storeDynamicBuffer(_dynamicUniformBuffersCamera,
                                                   size);
    ASSIGN_OR_RETURN(_dynamicDescriptorPool,
                     DescriptorPool::create(_logicalDevice, 1));
    ASSIGN_OR_RETURN(_dynamicDescriptorSet,
                     _dynamicDescriptorPool->createDesriptorSet(
                         _programManager.getVkDescriptorSetLayout(
                             DescriptorSetType::CAMERA)));
    _dynamicDescriptorSetWriter.writeDescriptorSet(
        _logicalDevice.getVkDevice(),
        _dynamicDescriptorSet.getVkDescriptorSet());

    ASSIGN_OR_RETURN(_lightBuffer,
                     Buffer::createUniformBuffer(_logicalDevice,
                                                 sizeof(UniformBufferLight)));
    _lightHandle = _bindlessWriter->storeBuffer(_lightBuffer);
    _ubLight.pos = glm::vec3(15.1891f, 2.66408f, -0.841221f);
    _ubLight.projView =
        glm::perspective(glm::radians(120.0f), 1.0f, 0.01f, 40.0f);
    _ubLight.projView[1][1] = -_ubLight.projView[1][1];
    _ubLight.projView =
        _ubLight.projView * glm::lookAt(_ubLight.pos,
                                        glm::vec3(-3.82383f, 3.66503f, 1.30751f),
                                        glm::vec3(0.0f, 1.0f, 0.0f));
    _lightBuffer.copyData(_ubLight, 0);

    return StatusOk();
  }

  Status createPresentResources() {
    static constexpr VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_4_BIT;
    if (_swapchainImageContexts.empty()) {
      return Error(EngineError::EMPTY_COLLECTION);
    }
    const VkFormat swapchainImageFormat =
        _swapchainImageContexts.cbegin()->second.format;
    AttachmentLayout attachmentsLayout(msaaSamples);
    attachmentsLayout
        .addColorResolvePresentAttachment(swapchainImageFormat,
                                          VK_ATTACHMENT_LOAD_OP_DONT_CARE)
        .addColorAttachment(swapchainImageFormat,
                            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                            VK_ATTACHMENT_STORE_OP_DONT_CARE)
        .addDepthAttachment(VK_FORMAT_D24_UNORM_S8_UINT,
                            VK_ATTACHMENT_STORE_OP_DONT_CARE);

    _renderpass = Renderpass(_logicalDevice, attachmentsLayout);
    RETURN_IF_ERROR(_renderpass.addSubpass({0, 1, 2}));
    _renderpass.addDependency(VK_SUBPASS_EXTERNAL, 0,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    RETURN_IF_ERROR(_renderpass.build());

    {
      SingleTimeCommandBuffer handle(*_singleTimeCommandPool);
      const VkCommandBuffer commandBuffer = handle.getCommandBuffer();
      for (auto &[swapchain, context] : _swapchainImageContexts) {
        context.framebuffers = lib::Buffer<Framebuffer>(context.views.size());
        for (uint8_t i = 0; i < context.views.size(); ++i) {
          ASSIGN_OR_RETURN(context.framebuffers[i],
                           Framebuffer::createFromSwapchain(
                               commandBuffer, _renderpass,
                               {context.width, context.height},
                               context.views[i], context.attachments));
        }
      }
    }
    static constexpr GraphicsPipelineParameters skyboxPipelineParameters = {
        .cullMode = VK_CULL_MODE_FRONT_BIT, .msaaSamples = msaaSamples};
    _graphicsPipelineSkybox = std::make_unique<GraphicsPipeline>(
        _renderpass, _skyboxShaderProgram, skyboxPipelineParameters);
    static constexpr GraphicsPipelineParameters pbrPipelineParameters = {
        .msaaSamples = msaaSamples,
        // .patchControlPoints = 3,
    };
    _graphicsPipeline = std::make_unique<GraphicsPipeline>(
        _renderpass, _pbrShaderProgram, pbrPipelineParameters);

    return StatusOk();
  }

  Status createShadowResources() {
    AttachmentLayout attachmentLayout;
    attachmentLayout.addShadowAttachment(
        VK_FORMAT_D32_SFLOAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    _shadowRenderPass = Renderpass(_logicalDevice, attachmentLayout);
    RETURN_IF_ERROR(_shadowRenderPass.addSubpass({0}));
    RETURN_IF_ERROR(_shadowRenderPass.build());
    ASSIGN_OR_RETURN(_shadowFramebuffer,
                     Framebuffer::createFromTextures(_shadowRenderPass,
                                                     std::span(&_shadowMap, 1)));

    static constexpr GraphicsPipelineParameters parameters = {
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .depthBiasConstantFactor = 0.7f,
        .depthBiasSlopeFactor = 2.0f,
    };
    _shadowPipeline = std::make_unique<GraphicsPipeline>(
        _shadowRenderPass, _shadowShaderProgram, parameters);
    return StatusOk();
  }

  Status createCommandBuffers() {
    for (auto &[swapchain, context] : _swapchainImageContexts) {
      for (int i = 0; i < MAX_THREADS_IN_POOL + 1; i++) {
        ASSIGN_OR_RETURN(context.commandPools[i],
                         CommandPool::create(_logicalDevice));
      }
      ASSIGN_OR_RETURN(
          context.primaryCommandBuffer,
          context.commandPools[MAX_THREADS_IN_POOL]
              ->createPrimaryCommandBuffers<MAX_FRAMES_IN_FLIGHT>());
      for (int i = 0; i < MAX_THREADS_IN_POOL; i++) {
        ASSIGN_OR_RETURN(
            context.commandBuffers[i],
            context.commandPools[i]
                ->createSecondaryCommandBuffers<MAX_FRAMES_IN_FLIGHT>());
      }
    }
    return StatusOk();
  }

  Status createSyncObjects() {
    static constexpr VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT};

    for (auto &[swapchain, context] : _swapchainImageContexts) {
      for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CHECK_VKCMD(vkCreateFence(_logicalDevice.getVkDevice(), &fenceInfo,
                                  nullptr, &context.fences[i]));
      }
    }
    return StatusOk();
  }

  glm::mat4 createProjectionMatrix(const XrFovf &fov, float near, float far) {
    const float tanLeft   = tanf(fov.angleLeft);
    const float tanRight  = tanf(fov.angleRight);
    const float tanDown   = tanf(fov.angleDown);
    const float tanUp     = tanf(fov.angleUp);

    const float tanWidth  = tanRight - tanLeft;
    const float tanHeight = tanDown - tanUp;

    glm::mat4 result(0.0f);
    result[0][0] = 2.0f / tanWidth;
    result[1][1] = 2.0f / tanHeight;
    result[2][0] = (tanRight + tanLeft) / tanWidth;
    result[2][1] = (tanUp + tanDown) / tanHeight;
    result[2][2] = -far / (far - near);
    result[2][3] = -1.0f;
    result[3][2] = -(far * near) / (far - near);

    return result;
  }

  glm::mat4 getViewMatrix(const XrPosef &pose) {
    const glm::quat orientation =
        glm::quat(pose.orientation.w, pose.orientation.x, pose.orientation.y,
                  pose.orientation.z);

    const glm::vec3 position =
        glm::vec3(pose.position.x, pose.position.y, pose.position.z);

    return glm::translate(glm::mat4_cast(glm::conjugate(orientation)), -position);
  }

  Status recordCommandBuffer(
      const XrCompositionLayerProjectionView &projectionLayerView,
      const Framebuffer &framebuffer,
      const glm::mat4& viewMatrix,
      const glm::mat4& projectionMatrix,
      const PrimaryCommandBuffer &primaryCommandBuffer,
      const SecondaryCommandBuffer &pbrCommandBuffer, const SecondaryCommandBuffer &skyCommandBuffer) {
    primaryCommandBuffer.begin();
    primaryCommandBuffer.beginRenderPass(framebuffer);

    static const bool viewportScissorInheritance =
        _physicalDevice->hasAvailableExtension(
            VK_NV_INHERITED_VIEWPORT_SCISSOR_EXTENSION_NAME);

    VkCommandBufferInheritanceViewportScissorInfoNV scissorViewportInheritance;
    if (viewportScissorInheritance) [[likely]] {
      scissorViewportInheritance = VkCommandBufferInheritanceViewportScissorInfoNV{
          .sType =
              VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_VIEWPORT_SCISSOR_INFO_NV,
          .viewportScissor2D = VK_TRUE,
          .viewportDepthCount = 1,
          .pViewportDepths = &framebuffer.getViewport(),
      };
    }

    std::future<Status> futures[2];
    futures[0] = std::async(std::launch::async, [&]() -> Status {
      const VkCommandBuffer commandBuffer =
          pbrCommandBuffer.getVkCommandBuffer();

      if (viewportScissorInheritance) [[likely]] {
        CHECK_VKCMD(
            pbrCommandBuffer.begin(framebuffer, &scissorViewportInheritance));
      } else {
        CHECK_VKCMD(pbrCommandBuffer.begin(framebuffer, nullptr));
        vkCmdSetViewport(commandBuffer, 0, 1, &framebuffer.getViewport());
        vkCmdSetScissor(commandBuffer, 0, 1, &framebuffer.getScissor());
      }

      vkCmdBindPipeline(commandBuffer,
                        _graphicsPipelineSkybox->getVkPipelineBindPoint(),
                        _graphicsPipelineSkybox->getVkPipeline());


      static const VkDescriptorSet descriptorSet =
          _bindlessDescriptorSet.getVkDescriptorSet();

      vkCmdBindPipeline(commandBuffer,
                        _graphicsPipeline->getVkPipelineBindPoint(),
                        _graphicsPipeline->getVkPipeline());

      const OctreeNode *root = _octree->getRoot();
      const auto &planes = extractFrustumPlanes(projectionMatrix *
          viewMatrix);

      VkDescriptorSet descriptorSets[] = {
          _bindlessDescriptorSet.getVkDescriptorSet(),
          _dynamicDescriptorSet.getVkDescriptorSet()};

      uint32_t offset;

      _dynamicDescriptorSetWriter.getDynamicBufferSizesWithOffsets(
          &offset, {_currentFrame});

      vkCmdBindDescriptorSets(commandBuffer,
                              _graphicsPipeline->getVkPipelineBindPoint(),
                              _graphicsPipeline->getVkPipelineLayout(), 0,
                              static_cast<uint32_t>(std::size(descriptorSets)),
                              descriptorSets, 1, &offset);

      recordOctreeSecondaryCommandBuffer(commandBuffer, root, planes);

      CHECK_VKCMD(vkEndCommandBuffer(commandBuffer));

      return StatusOk();
    });

    futures[1] = std::async(std::launch::async, [&]() -> Status {
      const VkCommandBuffer commandBuffer =
          skyCommandBuffer.getVkCommandBuffer();

      if (viewportScissorInheritance) [[likely]] {
        CHECK_VKCMD(
            skyCommandBuffer.begin(framebuffer, &scissorViewportInheritance));
      } else {
        CHECK_VKCMD(skyCommandBuffer.begin(framebuffer, nullptr));
        vkCmdSetViewport(commandBuffer, 0, 1, &framebuffer.getViewport());
        vkCmdSetScissor(commandBuffer, 0, 1, &framebuffer.getScissor());
      }

      vkCmdBindPipeline(commandBuffer,
                        _graphicsPipelineSkybox->getVkPipelineBindPoint(),
                        _graphicsPipelineSkybox->getVkPipeline());

      static constexpr VkDeviceSize offsets[] = {0};

      vkCmdBindVertexBuffers(commandBuffer, 0, 1,
                             &_vertexBufferCube.getVkBuffer(), offsets);

      vkCmdBindIndexBuffer(commandBuffer, _indexBufferCube.getVkBuffer(), 0,
                           _indexBufferCubeType);

      const PushConstantsSkybox pc = {
          .proj = projectionMatrix,
          .view = viewMatrix,
          .skyboxHandle = static_cast<uint32_t>(_skyboxHandle)};
      vkCmdPushConstants(
          commandBuffer, _graphicsPipelineSkybox->getVkPipelineLayout(),
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
          sizeof(pc), &pc);

      static const VkDescriptorSet descriptorSet =
          _bindlessDescriptorSet.getVkDescriptorSet();

      vkCmdBindDescriptorSets(commandBuffer,
                              _graphicsPipelineSkybox->getVkPipelineBindPoint(),
                              _graphicsPipelineSkybox->getVkPipelineLayout(), 0,
                              1, &descriptorSet, 0, nullptr);

      vkCmdDrawIndexed(commandBuffer,
                       _indexBufferCube.getSize() /
                           getIndexSize(_indexBufferCubeType),
                       1, 0, 0, 0);

      CHECK_VKCMD(vkEndCommandBuffer(commandBuffer));

      return StatusOk();
    });

    std::for_each(std::begin(futures), std::end(futures),
                  [](std::future<Status> &future) { future.wait(); });

    primaryCommandBuffer.executeSecondaryCommandBuffers(
        {pbrCommandBuffer.getVkCommandBuffer(), skyCommandBuffer.getVkCommandBuffer()});
    primaryCommandBuffer.endRenderPass();

    CHECK_VKCMD(primaryCommandBuffer.end());
    return StatusOk();
  }

  void recordShadowCommandBuffer(VkCommandBuffer commandBuffer,
                                              uint32_t imageIndex) {
    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    VkExtent2D extent = _shadowMap.getVkExtent2D();

    std::span<const VkClearValue> clearValues =
        _shadowRenderPass.getAttachmentsLayout().getVkClearValues();

    const VkRenderPassBeginInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = _shadowRenderPass.getVkRenderPass(),
        .framebuffer = _shadowFramebuffer.getVkFramebuffer(),
        .renderArea = {.offset = {0, 0}, .extent = extent},
        .clearValueCount = static_cast<uint32_t>(clearValues.size()),
        .pClearValues = clearValues.data()};

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport = {.x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    const VkRect2D scissor = {.offset = {0, 0}, .extent = extent};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkDeviceSize offsets[] = {0};
    vkCmdBindPipeline(commandBuffer, _shadowPipeline->getVkPipelineBindPoint(),
                      _shadowPipeline->getVkPipeline());

    PushConstantsShadow pc = {.lightProjView = _ubLight.projView};

    for (const Object &object : _objects) {
      const auto &meshComponent =
          _registry.getComponent<MeshComponent>(object.getEntity());
      const auto &transformComponent =
          _registry.getComponent<TransformComponent>(object.getEntity());

      pc.model = transformComponent.model;

      vkCmdPushConstants(commandBuffer, _shadowPipeline->getVkPipelineLayout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

      VkBuffer vertexBuffer = meshComponent.vertexBufferPrimitive.getVkBuffer();
      vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, offsets);

      const Buffer &indexBuffer = meshComponent.indexBuffer;
      vkCmdBindIndexBuffer(commandBuffer, indexBuffer.getVkBuffer(), 0,
                           meshComponent.indexType);

      vkCmdDrawIndexed(commandBuffer,
                       indexBuffer.getSize() /
                           getIndexSize(meshComponent.indexType),
                       1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
  }

  void recordOctreeSecondaryCommandBuffer(
      const VkCommandBuffer commandBuffer, const OctreeNode *rootNode,
      std::span<const glm::vec4> planes) {
    if (!rootNode || !rootNode->getVolume().intersectsFrustum(planes))
      return;

    static std::queue<const OctreeNode *> nodeQueue; // Keep it static to preserve
    // capacity
    nodeQueue.push(rootNode);

    while (!nodeQueue.empty()) {
      const OctreeNode *node = nodeQueue.front();
      nodeQueue.pop();

      for (const Object *object : node->getObjects()) {

        const auto &materialComponent =
            _registry.getComponent<MaterialComponent>(object->getEntity());
        const auto &transformComponent =
            _registry.getComponent<TransformComponent>(object->getEntity());

        const PushConstantsPBR pc = {
            .model = transformComponent.model,
            .light = (uint32_t)_lightHandle,
            .diffuse = (uint32_t)materialComponent.diffuse,
            .normal = (uint32_t)materialComponent.normal,
            .metallicRoughness = (uint32_t)materialComponent.metallicRoughness,
            .shadow = (uint32_t)_shadowHandle,
        };

        vkCmdPushConstants(
            commandBuffer, _graphicsPipeline->getVkPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(pc), &pc);

        const auto &meshComponent =
            _registry.getComponent<MeshComponent>(object->getEntity());
        const Buffer &indexBuffer = meshComponent.indexBuffer;
        const Buffer &vertexBuffer = meshComponent.vertexBuffer;
        static constexpr VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer.getVkBuffer(),
                               offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer.getVkBuffer(), 0,
                             meshComponent.indexType);
        vkCmdDrawIndexed(commandBuffer,
                         indexBuffer.getSize() /
                             getIndexSize(meshComponent.indexType),
                         1, 0, 0, 0);
      }

      static constexpr OctreeNode::Subvolume options[] = {
          OctreeNode::Subvolume::LOWER_LEFT_BACK,
          OctreeNode::Subvolume::LOWER_LEFT_FRONT,
          OctreeNode::Subvolume::LOWER_RIGHT_BACK,
          OctreeNode::Subvolume::LOWER_RIGHT_FRONT,
          OctreeNode::Subvolume::UPPER_LEFT_BACK,
          OctreeNode::Subvolume::UPPER_LEFT_FRONT,
          OctreeNode::Subvolume::UPPER_RIGHT_BACK,
          OctreeNode::Subvolume::UPPER_RIGHT_FRONT};

      for (OctreeNode::Subvolume option : options) {
        const OctreeNode *childNode = node->getChild(option);
        if (childNode && childNode->getVolume().intersectsFrustum(planes)) {
          nodeQueue.push(childNode);
        }
      }
    }
  }

  Status draw(const XrCompositionLayerProjectionView &projectionLayerView,
              uint32_t swapchain_image_index) override {
    const SwapchainContext &context =
        _swapchainImageContexts[projectionLayerView.subImage.swapchain];
    vkWaitForFences(_logicalDevice.getVkDevice(), 1,
                    &context.fences[_currentFrame], VK_TRUE, UINT64_MAX);

    XrSwapchainImageAcquireInfo acquire_info{};
    acquire_info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

    vkResetFences(_logicalDevice.getVkDevice(), 1,
                  &context.fences[_currentFrame]);

    context.primaryCommandBuffer[_currentFrame].resetCommandBuffer();
    for (int i = 0; i < MAX_THREADS_IN_POOL; i++)
      context.commandBuffers[i][_currentFrame].resetCommandBuffer();

    // Update uniform buffer:
    const XrVector3f &pos = projectionLayerView.pose.position;
    const glm::mat4 viewMatrix = getViewMatrix(projectionLayerView.pose);
    const glm::mat4 projectionMatrix = createProjectionMatrix(
        projectionLayerView.fov, 0.01f, 50.0f);
    _dynamicUniformBuffersCamera.copyData(
        UniformBufferCamera{.view = viewMatrix,
                            .proj = projectionMatrix,
                            .pos = {pos.x, pos.y, pos.z}},
        _currentFrame *
            _physicalDevice->getMemoryAlignment(sizeof(UniformBufferCamera)));

    recordCommandBuffer(projectionLayerView,
                        context.framebuffers[swapchain_image_index],
                        viewMatrix, projectionMatrix,
                        context.primaryCommandBuffer[_currentFrame],
                        context.commandBuffers[0][_currentFrame],
                        context.commandBuffers[1][_currentFrame]);

    VkSubmitInfo submitInfo = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};

    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.pWaitDstStageMask = waitStages;

    VkCommandBuffer submitCommands[] = {
        context.primaryCommandBuffer[_currentFrame].getVkCommandBuffer()};
    submitInfo.commandBufferCount =
        static_cast<uint32_t>(std::size(submitCommands));
    submitInfo.pCommandBuffers = submitCommands;

    CHECK_VKCMD(vkQueueSubmit(_logicalDevice.getGraphicsVkQueue(), 1,
                              &submitInfo, context.fences[_currentFrame]));

    if (++_currentFrame == MAX_FRAMES_IN_FLIGHT) {
      _currentFrame = 0;
    }
    return StatusOk();
  }
};

} // namespace xrw