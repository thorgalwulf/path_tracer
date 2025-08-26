// Copyright 2020-2024 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <nvh/fileoperations.hpp>  // For nvh::loadFile
#include <nvvk/context_vk.hpp>
#include <nvvk/error_vk.hpp>              // For NVVK_CHECK
#include <nvvk/resourceallocator_vk.hpp>  // For NVVK memory allocators
#include <nvvk/shaders_vk.hpp>            // For nvvk::createShaderModule

static const uint64_t render_width     = 800;
static const uint64_t render_height    = 600;
static const uint32_t workgroup_width  = 16;
static const uint32_t workgroup_height = 8;

int main(int argc, const char** argv)
{
  // Context
  // Create the Vulkan context, consisting of an instance, device, physical device, and queues.
  nvvk::ContextCreateInfo deviceInfo;  // Settings
  deviceInfo.apiMajor = 1;             // Specify the version of Vulkan we'll use
  deviceInfo.apiMinor = 2;
  // Required by KHR_acceleration_structure; allows work to be offloaded onto background threads and parallelized
  deviceInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  deviceInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false, &asFeatures);
  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  deviceInfo.addDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, false, &rayQueryFeatures);

  // Add the required device extensions for Debug Printf. If this is confusing,
  // don't worry; we'll remove this in the next chapter.
  deviceInfo.addDeviceExtension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
  VkValidationFeatureEnableEXT validationFeatureToEnable = VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT;
  VkValidationFeaturesEXT      validationInfo{.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                              .enabledValidationFeatureCount = 1,
                                              .pEnabledValidationFeatures    = &validationFeatureToEnable};
  deviceInfo.instanceCreateInfoExt = &validationInfo;
#ifdef _WIN32
  _putenv_s("DEBUG_PRINTF_TO_STDOUT", "1");
#else   // If not _WIN32
  static char putenvString[] = "DEBUG_PRINTF_TO_STDOUT=1";
  putenv(putenvString);
#endif  // _WIN32

  nvvk::Context context;     // Encapsulates device state in a single object
  context.init(deviceInfo);  // Initialize the context






  /* Allocator, Buffer, Command pool
  
    Because of the latency of the data transfer between the CPU and GPU, instead of potentially sending data between the CPU and GPU with every command,
    Vulkan's design encourages applications to batch collections of operations in command buffers.

    Allocate GPU memory using nvvk::ResourceAllocatorDedicated;
    Create a VkBuffer representing an image;
    Map data from the GPU to the CPU;
    Create a command buffer and a command pool;
    Record and submit a command buffer with a vkCmdFill command.
   */

  // Create the allocator
  nvvk::ResourceAllocatorDedicated allocator;
  allocator.init(context, context.m_physicalDevice);

  // Create a buffer
  VkDeviceSize       bufferSizeBytes = render_width * render_height * 3 * sizeof(float);
  VkBufferCreateInfo bufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size  = bufferSizeBytes,
                                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT means that the CPU can read this buffer's memory.
  // VK_MEMORY_PROPERTY_HOST_CACHED_BIT means that the CPU caches this memory.
  // VK_MEMORY_PROPERTY_HOST_COHERENT_BIT means that the CPU side of cache management
  // is handled automatically, with potentially slower reads/writes.
  nvvk::Buffer buffer = allocator.createBuffer(bufferCreateInfo,                         //
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT       //
                                                   | VK_MEMORY_PROPERTY_HOST_CACHED_BIT  //
                                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  const std::string        exePath(argv[0], std::string(argv[0]).find_last_of("/\\") + 1);
  std::vector<std::string> searchPaths = {exePath + PROJECT_RELDIRECTORY, exePath + PROJECT_RELDIRECTORY "..",
                                          exePath + PROJECT_RELDIRECTORY "../..", exePath + PROJECT_NAME};

  // Create the command pool
  VkCommandPoolCreateInfo cmdPoolInfo{.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,  //
                                      .queueFamilyIndex = context.m_queueGCT};
  VkCommandPool           cmdPool;
  NVVK_CHECK(vkCreateCommandPool(context, &cmdPoolInfo, nullptr, &cmdPool));

  // Shader loading and pipeline creation
  VkShaderModule rayTraceModule =
      nvvk::createShaderModule(context, nvh::loadFile("shaders/raytrace.comp.glsl.spv", true, searchPaths));

  // Describes the entrypoint and the stage to use for this shader module in the pipeline
  VkPipelineShaderStageCreateInfo shaderStageCreateInfo{.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .module = rayTraceModule,
                                                        .pName  = "main"};

  // For the moment, create an empty pipeline layout. You can ignore this code
  // for now; we'll replace it in the next chapter.
  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,  //
                                                      .setLayoutCount         = 0,                             //
                                                      .pushConstantRangeCount = 0};
  VkPipelineLayout           pipelineLayout;
  NVVK_CHECK(vkCreatePipelineLayout(context, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

  // Create the compute pipeline
  VkComputePipelineCreateInfo pipelineCreateInfo{.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,  //
                                                 .stage  = shaderStageCreateInfo,                           //
                                                 .layout = pipelineLayout};
  // Don't modify flags, basePipelineHandle, or basePipelineIndex
  VkPipeline computePipeline;
  NVVK_CHECK(vkCreateComputePipelines(context,                 // Device
                                      VK_NULL_HANDLE,          // Pipeline cache (uses default)
                                      1, &pipelineCreateInfo,  // Compute pipeline create info
                                      nullptr,                 // Allocator (uses default)
                                      &computePipeline));      // Output

  // Allocate a command buffer
  VkCommandBufferAllocateInfo cmdAllocInfo{.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool        = cmdPool,
                                           .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
  VkCommandBuffer             cmdBuffer;
  NVVK_CHECK(vkAllocateCommandBuffers(context, &cmdAllocInfo, &cmdBuffer));

  // Begin recording
  VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  NVVK_CHECK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

  // Bind the compute shader pipeline
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);

  // Run the compute shader with one workgroup for now
  vkCmdDispatch(cmdBuffer, 1, 1, 1);

  // Add a command that says "Make it so that memory writes by the compute shader
  // are available to read from the CPU." (In other words, "Flush the GPU caches
  // so the CPU can read the data.") To do this, we use a memory barrier.
  // This is one of the most complex parts of Vulkan, so don't worry if this is
  // confusing! We'll talk about pipeline barriers more in the extras.
  VkMemoryBarrier memoryBarrier{.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,  // Make shader writes
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};    // Readable by the CPU
  vkCmdPipelineBarrier(cmdBuffer,                                             // The command buffer
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,                  // From the compute shader
                       VK_PIPELINE_STAGE_HOST_BIT,                            // To the CPU
                       0,                                                     // No special flags
                       1, &memoryBarrier,                                     // An array of memory barriers
                       0, nullptr, 0, nullptr);                               // No other barriers

  // End recording
  NVVK_CHECK(vkEndCommandBuffer(cmdBuffer));

  // Submit the command buffer
  VkSubmitInfo submitInfo{.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,  //
                          .commandBufferCount = 1,                              //
                          .pCommandBuffers    = &cmdBuffer};
  NVVK_CHECK(vkQueueSubmit(context.m_queueGCT, 1, &submitInfo, VK_NULL_HANDLE));

  // Wait for the GPU to finish
  NVVK_CHECK(vkQueueWaitIdle(context.m_queueGCT));

  // Get the image data back from the GPU
  void* data = allocator.map(buffer);
  stbi_write_hdr("out.hdr", render_width, render_height, 3, reinterpret_cast<float*>(data));
  allocator.unmap(buffer);






  // Cleanup resources
  vkDestroyPipeline(context, computePipeline, nullptr);
  vkDestroyShaderModule(context, rayTraceModule, nullptr);
  vkDestroyPipelineLayout(context, pipelineLayout, nullptr);  // Will be removed in the next chapter
  vkFreeCommandBuffers(context, cmdPool, 1, &cmdBuffer);
  vkDestroyCommandPool(context, cmdPool, nullptr);
  allocator.destroy(buffer);
  allocator.deinit();
  context.deinit();  // Don't forget to clean up at the end of the program!
}
