#pragma once

#include "graphics_plugin_vulkan.h"

#include <android/asset_manager.h>

#include "common/file/android_file_loader.h"
#include "common/model_loader/model_loader.h"
#include "common/model_loader/obj_loader/obj_loader.h"
#include "vulkan_wrapper/descriptor_set/bindless_descriptor_set_writer.h"
#include "vulkan_wrapper/descriptor_set/descriptor_pool.h"
#include "vulkan_wrapper/memory_objects/texture.h"
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

} // namespace

namespace xrw {

class VulkanApplication : public GraphicsPluginVulkan {
public:
  VulkanApplication(PFN_vkDebugUtilsMessengerCallbackEXT debugCallback,
                    AAssetManager *assetManager,
                    const std::shared_ptr<FileLoader> &fileLoader)
      : GraphicsPluginVulkan(debugCallback), _assetManager(fileLoader),
        _programManager(fileLoader), _fileLoader(fileLoader) {}

  Status createResources() override {
    RETURN_IF_ERROR(loadCubemap());
    RETURN_IF_ERROR(createDescriptorSets());
    RETURN_IF_ERROR(createPresentResources());
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

  std::shared_ptr<DescriptorPool> _descriptorPool;
  DescriptorSet _bindlessDescriptorSet;
  std::unique_ptr<BindlessDescriptorSetWriter>
      _bindlessWriter; // Does not have to be unique_ptr

  Renderpass _renderpass;

  Status loadCubemap() {
    _assetManager.loadImageAsync(_logicalDevice,
                                 TEXTURES_PATH "cubemap_yokohama_rgba.ktx");

    ASSIGN_OR_RETURN(
        std::istringstream data,
        _fileLoader->loadFileToStringStream(MODELS_PATH "cube.obj"));
    ASSIGN_OR_RETURN(VertexData vertexDataCube, loadObj(data));
    _assetManager.loadVertexDataAsync(
        _logicalDevice, "cube.obj", vertexDataCube.indices,
        static_cast<uint8_t>(vertexDataCube.indexType),
        std::span<const glm::vec3>(vertexDataCube.positions));

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

  Status createDescriptorSets() {
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

    return StatusOk();
  }

  Status createPresentResources() {
    const VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_4_BIT;
    if (_swapchainImageContexts.empty()) {
      return Error(EngineError::EMPTY_COLLECTION);
    }
    const VkFormat swapchainImageFormat = _swapchainImageContexts.cbegin()->second.format;
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
      for (auto& [swapchain, context] : _swapchainImageContexts) {
        context.framebuffers = lib::Buffer<Framebuffer>(context.views.size());
        for (uint8_t i = 0; i < context.views.size(); ++i) {
          ASSIGN_OR_RETURN(Framebuffer framebuffer,
                           Framebuffer::createFromSwapchain(
                               commandBuffer, _renderpass, {context.width, context.height},
                               context.views[i],
                               context.attachments));
          context.framebuffers[i] = std::move(framebuffer);
        }

        static constexpr VkFenceCreateInfo fenceInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        for (uint8_t i = 0; i < context.fences.size(); ++i) {
          CHECK_VKCMD(vkCreateFence(_logicalDevice.getVkDevice(), &fenceInfo, nullptr,
                                    &context.fences[i]));
        }
      }
    }

    return StatusOk();
  }
};

} // namespace xrw