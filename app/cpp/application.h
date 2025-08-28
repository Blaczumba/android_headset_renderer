#pragma once

#include "graphics_plugin_vulkan.h"

#include <android/asset_manager.h>

#include "common/file/android_file_loader.h"
#include "vulkan_wrapper/memory_objects/texture.h"
#include "vulkan_wrapper/resource_manager/asset_manager.h"
#include "vulkan_wrapper/pipeline/shader_program.h"
#include "vulkan_wrapper/descriptor_set/descriptor_pool.h"
#include "vulkan_wrapper/descriptor_set/bindless_descriptor_set_writer.h"
#include "vulkan_wrapper/model_loader/model_loader.h"
#include "vulkan_wrapper/model_loader/obj_loader/obj_loader.h"

namespace {

ErrorOr<Texture> createCubemap(const LogicalDevice &logicalDevice,
                               VkCommandBuffer commandBuffer,
                               VkBuffer stagingBuffer,
                               const ImageDimensions &dimensions,
                               VkFormat format, float samplerAnisotropy) {
  return TextureBuilder()
      .withAspect(VK_IMAGE_ASPECT_COLOR_BIT)
      .withExtent(dimensions.width, dimensions.height)
      .withFormat(format)
      .withMipLevels(dimensions.mipLevels)
      .withUsage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
      .withLayerCount(6)
      .withMaxAnisotropy(samplerAnisotropy)
      .withMaxLod(static_cast<float>(dimensions.mipLevels))
      .withLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      .buildImage(logicalDevice, commandBuffer, stagingBuffer,
                  dimensions.copyRegions);
}

} // namespace

namespace xrw {

class VulkanApplication : public GraphicsPluginVulkan {
 public:
  VulkanApplication(PFN_vkDebugUtilsMessengerCallbackEXT debugCallback, AAssetManager* assetManager)
      : GraphicsPluginVulkan(debugCallback), _assetManager(std::make_unique<AndroidFileLoader>(assetManager)) {}

  Status initialize(XrInstance xrInstance, XrSystemId systemId) override {
    RETURN_IF_ERROR(GraphicsPluginVulkan::initialize(xrInstance, systemId));
    RETURN_IF_ERROR(loadCubemap());
    RETURN_IF_ERROR(createDescriptorSets());
    return StatusOk();
  }

 private:
  AssetManager _assetManager;
  ShaderProgramManager _programManager;

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

    Status loadCubemap() {
      std::string_view a = TEXTURES_PATH;
      _assetManager.loadImageCubemapAsync(_logicalDevice, TEXTURES_PATH
      "cubemap_yokohama_rgba.ktx");

      ASSIGN_OR_RETURN(VertexData vertexDataCube, loadObj(MODELS_PATH "cube.obj"));
      _assetManager.loadVertexDataAsync(
          _logicalDevice, "cube.obj", vertexDataCube.indices,
          static_cast<uint8_t>(vertexDataCube.indexType),
          std::span<const glm::vec3>(vertexDataCube.positions));

      {
        SingleTimeCommandBuffer handle(*_singleTimeCommandPool);
        const VkCommandBuffer commandBuffer = handle.getCommandBuffer();

        // Load texture.
        ASSIGN_OR_RETURN(
            const AssetManager::ImageData &imgData,
            _assetManager.getImageData(TEXTURES_PATH "cubemap_yokohama_rgba.ktx"));
        ASSIGN_OR_RETURN(_textureCubemap,
                         createCubemap(_logicalDevice, commandBuffer,
                                       imgData.stagingBuffer.getVkBuffer(),
                                       imgData.imageDimensions,
                                       VK_FORMAT_R8G8B8A8_UNORM,
                                       _physicalDevice->getMaxSamplerAnisotropy()));

        // Load geometry.
        ASSIGN_OR_RETURN(const AssetManager::VertexData &vData,
                         _assetManager.getVertexData("cube.obj"));
        ASSIGN_OR_RETURN(
            _vertexBufferCube,
            Buffer::createVertexBuffer(_logicalDevice,
                                       vData.vertexBufferPositions.getSize()));
        RETURN_IF_ERROR(_vertexBufferCube.copyBuffer(commandBuffer,
                                                     vData.vertexBufferPositions));
        ASSIGN_OR_RETURN(
            _indexBufferCube,
            Buffer::createIndexBuffer(_logicalDevice, vData.indexBuffer.getSize()));
        RETURN_IF_ERROR(
            _indexBufferCube.copyBuffer(commandBuffer, vData.indexBuffer));
        _indexBufferCubeType = vData.indexType;
      }

      return StatusOk();
    }

  Status createDescriptorSets() {
    ASSIGN_OR_RETURN(_skyboxShaderProgram,
                     _programManager.createSkyboxProgram(_logicalDevice));
    ASSIGN_OR_RETURN(_bindlessDescriptorSet,
                     _descriptorPool->createDesriptorSet(
                         _programManager.getVkDescriptorSetLayout(
                             DescriptorSetType::BINDLESS)));
    _bindlessWriter =
        std::make_unique<BindlessDescriptorSetWriter>(_bindlessDescriptorSet);
    _skyboxHandle = _bindlessWriter->storeTexture(_textureCubemap);

    return StatusOk();
  }
};

} // xrw