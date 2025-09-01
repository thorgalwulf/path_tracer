#include <array>
#include <cassert>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
/*
The OBJ file format represents meshes using an array of vertices (which are 3D points, but can also have some other attributes, such as a color per vertex, that we won't use), 
and an array of sets of three indices. Each set of three indices corresponds to three vertices, which represent a triangle.

Vulkan ray tracing uses a two-level acceleration structure format. Bottom-level acceleration structures (BLASes) are acceleration structures of triangles 
(or of bounding boxes of procedural objects), and top-level acceleration structures are acceleration structures of instances, each of which point to a BLAS, 
include a transform (describing the position, rotation, translation, and skew of the instance using a 3 � 4 affine transformation matrix).

We can determine where the ray intersected the triangle. More specifically, barycentric coordinates tell us where the intersection is, relative to the vertices of the triangle.
scratchapixel.com - Barycentric Coordinates of a Triangle
intersection of the ray at point p of the triangle: P = (1-u-v)*v0 + u*v1 + v*v2
Each point on the triangle has a unique set of barycentric coordinates. The coordinates range from 0 to 1, and so do the color channels. 
This creates a color gradient across the surface of the triangle, where each point's color is directly related to its position. 
The color at any given pixel therefore indicates exactly where on the triangle the ray hit.
*/

#include <nvh/fileoperations.hpp>         // For nvh::loadFile
#include <nvvk/context_vk.hpp>
#include <nvvk/descriptorsets_vk.hpp>     // For nvvk::DescriptorSetContainer
#include <nvvk/error_vk.hpp>              // For NVVK_CHECK
#include <nvvk/raytraceKHR_vk.hpp>        // For nvvk::RaytracingBuilderKHR
#include <nvvk/resourceallocator_vk.hpp>  // For NVVK memory allocators
#include <nvvk/shaders_vk.hpp>            // For nvvk::createShaderModule





static const uint64_t render_width     = 800;
static const uint64_t render_height    = 600;
static const uint32_t workgroup_width  = 16;
static const uint32_t workgroup_height = 8;





// second command buffer to upload vertex and index data to the GPU
VkCommandBuffer AllocateAndBeginOneTimeCommandBuffer(VkDevice device, VkCommandPool cmdPool)
{
    VkCommandBufferAllocateInfo cmdAllocInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = cmdPool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1 };
    VkCommandBuffer cmdBuffer;
    NVVK_CHECK(vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuffer));
    VkCommandBufferBeginInfo beginInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    NVVK_CHECK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));
    return cmdBuffer;
}




// function that ends recording a command buffer, then submits it to a queue, waits for it to finish, and then frees the command buffer
void EndSubmitWaitAndFreeCommandBuffer(VkDevice device, VkQueue queue, VkCommandPool cmdPool, VkCommandBuffer& cmdBuffer)
{
    NVVK_CHECK(vkEndCommandBuffer(cmdBuffer));
    VkSubmitInfo submitInfo{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmdBuffer };
    NVVK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
    NVVK_CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuffer);
}





// Function that gets the device address of a VkBuffer. A device address is like the address of a piece of memory on the GPU.
VkDeviceAddress GetBufferDeviceAddress(VkDevice device, VkBuffer buffer)
{
    VkBufferDeviceAddressInfo addressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer };
    return vkGetBufferDeviceAddress(device, &addressInfo);
}





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

  nvvk::Context context;     // Encapsulates device state in a single object
  context.init(deviceInfo);  // Initialize the context






  
  // Allocator
  // Create the allocator
  nvvk::ResourceAllocatorDedicated allocator;
  allocator.init(context, context.m_physicalDevice);





  // Buffer
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

  



  // Load the mesh of the first shape from an OBJ file
  const std::string        exePath(argv[0], std::string(argv[0]).find_last_of("/\\") + 1);
  std::vector<std::string> searchPaths = { exePath + PROJECT_RELDIRECTORY, exePath + PROJECT_RELDIRECTORY "..",
                                          exePath + PROJECT_RELDIRECTORY "../..", exePath + PROJECT_NAME };
  tinyobj::ObjReader       reader;  // Used to read an OBJ file
  reader.ParseFromFile(nvh::findFile("scenes/CornellBox-Original-Merged.obj", searchPaths));
  assert(reader.Valid());  // Make sure tinyobj was able to parse this file

  // Get the vertices and indices of the OBJ file
  const std::vector<tinyobj::real_t>   objVertices = reader.GetAttrib().GetVertices();
  const std::vector<tinyobj::shape_t>& objShapes = reader.GetShapes();  // All shapes in the file
  assert(objShapes.size() == 1);                                          // Check that this file has only one shape (the mesh formed by triangles)
  const tinyobj::shape_t& objShape = objShapes[0];                        // Get the first shape
  // Get the indices of the vertices of the first mesh of `objShape` in `attrib.vertices`:
  std::vector<uint32_t> objIndices;
  objIndices.reserve(objShape.mesh.indices.size());
  for (const tinyobj::index_t& index : objShape.mesh.indices)
  {
      objIndices.push_back(index.vertex_index);
  }
  // for (auto x : objIndices) printf("%d ", x);





  // Command Pool
  // Create the command pool
  VkCommandPoolCreateInfo cmdPoolInfo{.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,  //
                                      .queueFamilyIndex = context.m_queueGCT};
  VkCommandPool           cmdPool;
  NVVK_CHECK(vkCreateCommandPool(context, &cmdPoolInfo, nullptr, &cmdPool));



  
  
  // Upload the vertex and index buffers to the GPU.
  nvvk::Buffer vertexBuffer, indexBuffer;
  {
      // Start a command buffer for uploading the buffers
      VkCommandBuffer uploadCmdBuffer = AllocateAndBeginOneTimeCommandBuffer(context, cmdPool);

      // We get these buffers' device addresses, and use them as storage buffers and build inputs.
      const VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
          | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

      vertexBuffer = allocator.createBuffer(uploadCmdBuffer, objVertices, usage);
      indexBuffer = allocator.createBuffer(uploadCmdBuffer, objIndices, usage);

	  // End the command buffer, submit it, and wait for it to finish
      EndSubmitWaitAndFreeCommandBuffer(context, context.m_queueGCT, cmdPool, uploadCmdBuffer);
      // Free the memory of the allocator: the allocator also allocates some temporary staging memory to perform these uploads to GPU-local memory
      allocator.finalizeAndReleaseStaging();
  }

  // Describe the bottom-level acceleration structure (BLAS)
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> blases;
  {
      nvvk::RaytracingBuilderKHR::BlasInput blas;
      // Get the device addresses of the vertex and index buffers
      VkDeviceAddress vertexBufferAddress = GetBufferDeviceAddress(context, vertexBuffer.buffer);
      VkDeviceAddress indexBufferAddress = GetBufferDeviceAddress(context, indexBuffer.buffer);
      // Specify where the builder can find the vertices and indices for triangles, and their formats:
      VkAccelerationStructureGeometryTrianglesDataKHR triangles{
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
          .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
          .vertexData = {.deviceAddress = vertexBufferAddress},
          .vertexStride = 3 * sizeof(float),
          .maxVertex = static_cast<uint32_t>(objVertices.size() / 3 - 1),
          .indexType = VK_INDEX_TYPE_UINT32,
          .indexData = {.deviceAddress = indexBufferAddress},
          .transformData = {.deviceAddress = 0}  // No transform
  };
  
   // Create a VkAccelerationStructureGeometryKHR object that says it handles opaque triangles and points to the above:
   VkAccelerationStructureGeometryKHR geometry{ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                               .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                                               .geometry = {.triangles = triangles},
                                               .flags = VK_GEOMETRY_OPAQUE_BIT_KHR };
   blas.asGeometry.push_back(geometry);
   // Create offset info that allows us to say how many triangles and vertices to read
   VkAccelerationStructureBuildRangeInfoKHR offsetInfo{
       .primitiveCount = static_cast<uint32_t>(objIndices.size() / 3),  // Number of triangles
       .primitiveOffset = 0,                                             // Offset added when looking up triangles
       .firstVertex = 0,  // Offset added when looking up vertices in the vertex buffer
       .transformOffset = 0   // Offset added when looking up transformation matrices, if we used them
   };
   blas.asBuildOffsetInfo.push_back(offsetInfo);
   blases.push_back(blas);
  }
  // Create the BLAS
  nvvk::RaytracingBuilderKHR raytracingBuilder;
  raytracingBuilder.setup(context, &allocator, context.m_queueGCT);
  raytracingBuilder.buildBlas(blases, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);

  // Create an instance pointing to this BLAS, and build it into a TLAS:
  std::vector<VkAccelerationStructureInstanceKHR> instances;
  {
      VkAccelerationStructureInstanceKHR instance{};
      instance.accelerationStructureReference = raytracingBuilder.getBlasDeviceAddress(0);  // The address of the BLAS in `blases` that this instance points to
      // Set the instance transform to the identity matrix:
      instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
      instance.instanceCustomIndex = 0;  // 24 bits accessible to ray shaders via rayQueryGetIntersectionInstanceCustomIndexEXT
      // Used for a shader offset index, accessible via rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT
      instance.instanceShaderBindingTableRecordOffset = 0;
      instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;  // How to trace this instance
      instance.mask = 0xFF;
      instances.push_back(instance);
  }
  raytracingBuilder.buildTlas(instances, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);





  // Descriptor Set
  
  // Here's the list of bindings for the descriptor set layout, from raytrace.comp.glsl:
  // 0 - a storage buffer (the buffer `buffer`)
  // 1 - an acceleration structure (the TLAS)
  // To trace rays from a shader, we need to add the acceleration structure to the descriptor set.
  nvvk::DescriptorSetContainer descriptorSetContainer(context);
  descriptorSetContainer.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  descriptorSetContainer.addBinding(1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  descriptorSetContainer.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  descriptorSetContainer.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  // Create a layout from the list of bindings
  descriptorSetContainer.initLayout();
  // Create a descriptor pool from the list of bindings with space for 1 set, and allocate that set
  descriptorSetContainer.initPool(1);
  // Create a simple pipeline layout from the descriptor set layout:
  descriptorSetContainer.initPipeLayout();

  // Write values into the descriptor set.
  
  // Make this descriptor in the descriptor set point to the TLAS
  // Add storage buffer descriptors 2 and 3 for the vertex and index buffers: read mesh data from triangle intersections (triangle vertices)
  std::array<VkWriteDescriptorSet, 4> writeDescriptorSets;
  // 0
  VkDescriptorBufferInfo descriptorBufferInfo{ .buffer = buffer.buffer,    // The VkBuffer object
                                              .range = bufferSizeBytes };  // The length of memory to bind; offset is 0.
  writeDescriptorSets[0] = descriptorSetContainer.makeWrite(0 /*set index*/, 0 /*binding*/, &descriptorBufferInfo);
  // 1
  VkAccelerationStructureKHR tlasCopy = raytracingBuilder.getAccelerationStructure();  // So that we can take its address
  VkWriteDescriptorSetAccelerationStructureKHR descriptorAS{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                            .accelerationStructureCount = 1,
                                                            .pAccelerationStructures = &tlasCopy };
  writeDescriptorSets[1] = descriptorSetContainer.makeWrite(0, 1, &descriptorAS);
  // 2
  VkDescriptorBufferInfo vertexDescriptorBufferInfo{ .buffer = vertexBuffer.buffer, .range = VK_WHOLE_SIZE };
  writeDescriptorSets[2] = descriptorSetContainer.makeWrite(0, 2, &vertexDescriptorBufferInfo);
  // 3
  VkDescriptorBufferInfo indexDescriptorBufferInfo{ .buffer = indexBuffer.buffer, .range = VK_WHOLE_SIZE };
  writeDescriptorSets[3] = descriptorSetContainer.makeWrite(0, 3, &indexDescriptorBufferInfo);
  vkUpdateDescriptorSets(context,                                           // The context
      static_cast<uint32_t>(writeDescriptorSets.size()),                    // Number of VkWriteDescriptorSet objects
      writeDescriptorSets.data(),                                           // Pointer to VkWriteDescriptorSet objects
      0, nullptr);                                                          // An array of VkCopyDescriptorSet objects (unused)




  
  // Shader loading and pipeline creation
  VkShaderModule rayTraceModule =
      nvvk::createShaderModule(context, nvh::loadFile("shaders/raytrace.comp.glsl.spv", true, searchPaths));

  // Describes the entrypoint and the stage to use for this shader module in the pipeline
  VkPipelineShaderStageCreateInfo shaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .module = rayTraceModule,
                                                        .pName = "main" };

  // Create the compute pipeline
  VkComputePipelineCreateInfo pipelineCreateInfo{ .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                 .stage = shaderStageCreateInfo,
                                                 .layout = descriptorSetContainer.getPipeLayout() };
  // Don't modify flags, basePipelineHandle, or basePipelineIndex
  VkPipeline computePipeline;
  NVVK_CHECK(vkCreateComputePipelines(context,                 // Device
      VK_NULL_HANDLE,          // Pipeline cache (uses default)
      1, &pipelineCreateInfo,  // Compute pipeline create info
      nullptr,                 // Allocator (uses default)
      &computePipeline));      // Output





  // Command Buffer
  // Allocate a command buffer
  // Create and start recording a command buffer
  VkCommandBuffer cmdBuffer = AllocateAndBeginOneTimeCommandBuffer(context, cmdPool);





  // Binding
  // Bind the compute shader pipeline
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);

  // Bind the descriptor set
  VkDescriptorSet descriptorSet = descriptorSetContainer.getSet(0);
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, descriptorSetContainer.getPipeLayout(), 0, 1,
      &descriptorSet, 0, nullptr);
  



  // Dispatch
  // Run the compute shader with enough workgroups to cover the entire buffer:
  vkCmdDispatch(cmdBuffer, (uint32_t(render_width) + workgroup_width - 1) / workgroup_width,
      (uint32_t(render_height) + workgroup_height - 1) / workgroup_height, 1);





  // Memory Barrier
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





  // Finishing operations
  // End and submit the command buffer, then wait for it to finish:
  EndSubmitWaitAndFreeCommandBuffer(context, context.m_queueGCT, cmdPool, cmdBuffer);

  // Get the image data back from the GPU
  void* data = allocator.map(buffer);
  stbi_write_hdr("out.hdr", render_width, render_height, 3, reinterpret_cast<float*>(data));
  allocator.unmap(buffer);






  // Cleanup
  vkDestroyPipeline(context, computePipeline, nullptr);
  vkDestroyShaderModule(context, rayTraceModule, nullptr);
  descriptorSetContainer.deinit();
  raytracingBuilder.destroy();
  allocator.destroy(vertexBuffer);
  allocator.destroy(indexBuffer);
  vkDestroyCommandPool(context, cmdPool, nullptr);
  allocator.destroy(buffer);
  allocator.deinit();
  context.deinit();
}
