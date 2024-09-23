/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <sstream>

#include "GHOST_C-api.h"

#include "BLI_threads.h"

#include "CLG_log.h"

#include "gpu_capabilities_private.hh"
#include "gpu_platform_private.hh"

#include "vk_batch.hh"
#include "vk_context.hh"
#include "vk_drawlist.hh"
#include "vk_fence.hh"
#include "vk_framebuffer.hh"
#include "vk_index_buffer.hh"
#include "vk_pixel_buffer.hh"
#include "vk_query.hh"
#include "vk_shader.hh"
#include "vk_state_manager.hh"
#include "vk_storage_buffer.hh"
#include "vk_texture.hh"
#include "vk_uniform_buffer.hh"
#include "vk_vertex_buffer.hh"

#include "vk_backend.hh"

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace blender::gpu {

static Vector<StringRefNull> missing_capabilities_get(VkPhysicalDevice vk_physical_device)
{
  Vector<StringRefNull> missing_capabilities;
  /* Check device features. */
  VkPhysicalDeviceFeatures2 features = {};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = {};

  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
  VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT
      dynamic_rendering_unused_attachments = {};
  dynamic_rendering_unused_attachments.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
  features.pNext = &dynamic_rendering;
  dynamic_rendering.pNext = &dynamic_rendering_unused_attachments;

  vkGetPhysicalDeviceFeatures2(vk_physical_device, &features);
#ifndef __APPLE__
  if (features.features.geometryShader == VK_FALSE) {
    missing_capabilities.append("geometry shaders");
  }
  if (features.features.logicOp == VK_FALSE) {
    missing_capabilities.append("logical operations");
  }
#endif
  if (features.features.dualSrcBlend == VK_FALSE) {
    missing_capabilities.append("dual source blending");
  }
  if (features.features.imageCubeArray == VK_FALSE) {
    missing_capabilities.append("image cube array");
  }
  if (features.features.multiDrawIndirect == VK_FALSE) {
    missing_capabilities.append("multi draw indirect");
  }
  if (features.features.multiViewport == VK_FALSE) {
    missing_capabilities.append("multi viewport");
  }
  if (features.features.shaderClipDistance == VK_FALSE) {
    missing_capabilities.append("shader clip distance");
  }
  if (features.features.drawIndirectFirstInstance == VK_FALSE) {
    missing_capabilities.append("draw indirect first instance");
  }
  if (features.features.fragmentStoresAndAtomics == VK_FALSE) {
    missing_capabilities.append("fragment stores and atomics");
  }
  if (dynamic_rendering.dynamicRendering == VK_FALSE) {
    missing_capabilities.append("dynamic rendering");
  }

  /* Check device extensions. */
  uint32_t vk_extension_count;
  vkEnumerateDeviceExtensionProperties(vk_physical_device, nullptr, &vk_extension_count, nullptr);

  Array<VkExtensionProperties> vk_extensions(vk_extension_count);
  vkEnumerateDeviceExtensionProperties(
      vk_physical_device, nullptr, &vk_extension_count, vk_extensions.data());
  Set<StringRefNull> extensions;
  for (VkExtensionProperties &vk_extension : vk_extensions) {
    extensions.add(vk_extension.extensionName);
  }

  if (!extensions.contains(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    missing_capabilities.append(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }
  if (!extensions.contains(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)) {
    missing_capabilities.append(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
  }
  if (!extensions.contains(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)) {
    missing_capabilities.append(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
  }
  if (!extensions.contains(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
    missing_capabilities.append(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  }

  return missing_capabilities;
}

bool VKBackend::is_supported()
{
  CLG_logref_init(&LOG);

  /* Initialize an vulkan 1.2 instance. */
  VkApplicationInfo vk_application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  vk_application_info.pApplicationName = "Blender";
  vk_application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  vk_application_info.pEngineName = "Blender";
  vk_application_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  vk_application_info.apiVersion = VK_API_VERSION_1_2;

  const char *instance_extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};

  VkInstanceCreateInfo vk_instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vk_instance_info.pApplicationInfo = &vk_application_info;
  vk_instance_info.enabledExtensionCount = 1;
  vk_instance_info.ppEnabledExtensionNames = instance_extensions;

  VkInstance vk_instance = VK_NULL_HANDLE;
  vkCreateInstance(&vk_instance_info, nullptr, &vk_instance);
  if (vk_instance == VK_NULL_HANDLE) {
    CLOG_ERROR(&LOG, "Unable to initialize a Vulkan 1.2 instance.");
    return false;
  }

  // go over all the devices
  uint32_t physical_devices_count = 0;
  vkEnumeratePhysicalDevices(vk_instance, &physical_devices_count, nullptr);
  Array<VkPhysicalDevice> vk_physical_devices(physical_devices_count);
  vkEnumeratePhysicalDevices(vk_instance, &physical_devices_count, vk_physical_devices.data());

  for (VkPhysicalDevice vk_physical_device : vk_physical_devices) {
    Vector<StringRefNull> missing_capabilities = missing_capabilities_get(vk_physical_device);

    VkPhysicalDeviceProperties vk_properties = {};
    vkGetPhysicalDeviceProperties(vk_physical_device, &vk_properties);

    /* Report result. */
    if (missing_capabilities.is_empty()) {
      /* This device meets minimum requirements. */
      CLOG_INFO(&LOG,
                0,
                "Device [%s] supports minimum requirements. Skip checking other GPUs. Another GPU "
                "can still be selected during auto-detection.",
                vk_properties.deviceName);

      vkDestroyInstance(vk_instance, nullptr);
      return true;
    }

    std::stringstream ss;
    ss << "Device [" << vk_properties.deviceName
       << "] does not meet minimum requirements. Missing features are [";
    for (StringRefNull &feature : missing_capabilities) {
      ss << feature << ", ";
    }
    ss.seekp(-2, std::ios_base::end);
    ss << "]";
    CLOG_WARN(&LOG, "%s", ss.str().c_str());
  }

  /* No device found meeting the minimum requirements. */

  vkDestroyInstance(vk_instance, nullptr);
  CLOG_ERROR(&LOG,
             "No Vulkan device found that meets the minimum requirements. "
             "Updating GPU driver can improve compatibility.");
  return false;
}

static eGPUOSType determine_os_type()
{
#ifdef _WIN32
  return GPU_OS_WIN;
#elif defined(__APPLE__)
  return GPU_OS_MAC;
#else
  return GPU_OS_UNIX;
#endif
}

void VKBackend::platform_init()
{
  GPG.init(GPU_DEVICE_ANY,
           determine_os_type(),
           GPU_DRIVER_ANY,
           GPU_SUPPORT_LEVEL_SUPPORTED,
           GPU_BACKEND_VULKAN,
           "",
           "",
           "",
           GPU_ARCHITECTURE_IMR);

  /* Query for all compatible devices */
  VkApplicationInfo vk_application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  vk_application_info.pApplicationName = "Blender";
  vk_application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  vk_application_info.pEngineName = "Blender";
  vk_application_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  vk_application_info.apiVersion = VK_API_VERSION_1_2;

  const char *instance_extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};

  VkInstanceCreateInfo vk_instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vk_instance_info.pApplicationInfo = &vk_application_info;
  vk_instance_info.enabledExtensionCount = 1;
  vk_instance_info.ppEnabledExtensionNames = instance_extensions;

  VkInstance vk_instance = VK_NULL_HANDLE;
  vkCreateInstance(&vk_instance_info, nullptr, &vk_instance);
  BLI_assert(vk_instance != VK_NULL_HANDLE);

  uint32_t physical_devices_count = 0;
  vkEnumeratePhysicalDevices(vk_instance, &physical_devices_count, nullptr);
  Array<VkPhysicalDevice> vk_physical_devices(physical_devices_count);
  vkEnumeratePhysicalDevices(vk_instance, &physical_devices_count, vk_physical_devices.data());
  int index = 0;
  for (VkPhysicalDevice vk_physical_device : vk_physical_devices) {
    if (missing_capabilities_get(vk_physical_device).is_empty()) {
      VkPhysicalDeviceProperties vk_properties = {};
      vkGetPhysicalDeviceProperties(vk_physical_device, &vk_properties);
      std::stringstream identifier;
      identifier << std::hex << vk_properties.vendorID << "/" << vk_properties.deviceID << "/"
                 << index;
      GPG.devices.append({identifier.str(),
                          index,
                          vk_properties.vendorID,
                          vk_properties.deviceID,
                          std::string(vk_properties.deviceName)});
    }
    index++;
  }
  vkDestroyInstance(vk_instance, nullptr);
  std::sort(GPG.devices.begin(), GPG.devices.end(), [&](const GPUDevice &a, const GPUDevice &b) {
    if (a.name == b.name) {
      return a.index < b.index;
    }
    return a.name < b.name;
  });
}

void VKBackend::platform_init(const VKDevice &device)
{
  const VkPhysicalDeviceProperties &properties = device.physical_device_properties_get();

  eGPUDeviceType device_type = device.device_type();
  eGPUOSType os = determine_os_type();
  eGPUDriverType driver = GPU_DRIVER_ANY;
  eGPUSupportLevel support_level = GPU_SUPPORT_LEVEL_SUPPORTED;

  std::string vendor_name = device.vendor_name();
  std::string driver_version = device.driver_version();

  GPG.init(device_type,
           os,
           driver,
           support_level,
           GPU_BACKEND_VULKAN,
           vendor_name.c_str(),
           properties.deviceName,
           driver_version.c_str(),
           GPU_ARCHITECTURE_IMR);
}

void VKBackend::detect_workarounds(VKDevice &device)
{
  VKWorkarounds workarounds;

  if (G.debug & G_DEBUG_GPU_FORCE_WORKAROUNDS) {
    printf("\n");
    printf("VK: Forcing workaround usage and disabling features and extensions.\n");
    printf("    Vendor: %s\n", device.vendor_name().c_str());
    printf("    Device: %s\n", device.physical_device_properties_get().deviceName);
    printf("    Driver: %s\n", device.driver_version().c_str());
    /* Force workarounds. */
    workarounds.not_aligned_pixel_formats = true;
    workarounds.shader_output_layer = true;
    workarounds.shader_output_viewport_index = true;
    workarounds.vertex_formats.r8g8b8 = true;

    device.workarounds_ = workarounds;
    return;
  }

  workarounds.shader_output_layer =
      !device.physical_device_vulkan_12_features_get().shaderOutputLayer;
  workarounds.shader_output_viewport_index =
      !device.physical_device_vulkan_12_features_get().shaderOutputViewportIndex;

  /* AMD GPUs don't support texture formats that use are aligned to 24 or 48 bits. */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_ANY) ||
      GPU_type_matches(GPU_DEVICE_APPLE, GPU_OS_MAC, GPU_DRIVER_ANY))
  {
    workarounds.not_aligned_pixel_formats = true;
  }

  VkFormatProperties format_properties = {};
  vkGetPhysicalDeviceFormatProperties(
      device.physical_device_get(), VK_FORMAT_R8G8B8_UNORM, &format_properties);
  workarounds.vertex_formats.r8g8b8 = (format_properties.bufferFeatures &
                                       VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0;

  device.workarounds_ = workarounds;
}

void VKBackend::platform_exit()
{
  GPG.clear();
  VKDevice &device = VKBackend::get().device;
  if (device.is_initialized()) {
    device.deinit();
  }
}

void VKBackend::delete_resources() {}

void VKBackend::samplers_update()
{
  VKDevice &device = VKBackend::get().device;
  if (device.is_initialized()) {
    device.reinit();
  }
}

void VKBackend::compute_dispatch(int groups_x_len, int groups_y_len, int groups_z_len)
{
  VKContext &context = *VKContext::get();
  render_graph::VKResourceAccessInfo &resources = context.update_and_get_access_info();
  render_graph::VKDispatchNode::CreateInfo dispatch_info(resources);
  context.update_pipeline_data(dispatch_info.dispatch_node.pipeline_data);
  dispatch_info.dispatch_node.group_count_x = groups_x_len;
  dispatch_info.dispatch_node.group_count_y = groups_y_len;
  dispatch_info.dispatch_node.group_count_z = groups_z_len;
  context.render_graph.add_node(dispatch_info);
}

void VKBackend::compute_dispatch_indirect(StorageBuf *indirect_buf)
{
  BLI_assert(indirect_buf);
  VKContext &context = *VKContext::get();
  VKStorageBuffer &indirect_buffer = *unwrap(indirect_buf);
  render_graph::VKResourceAccessInfo &resources = context.update_and_get_access_info();
  render_graph::VKDispatchIndirectNode::CreateInfo dispatch_indirect_info(resources);
  context.update_pipeline_data(dispatch_indirect_info.dispatch_indirect_node.pipeline_data);
  dispatch_indirect_info.dispatch_indirect_node.buffer = indirect_buffer.vk_handle();
  dispatch_indirect_info.dispatch_indirect_node.offset = 0;
  context.render_graph.add_node(dispatch_indirect_info);
}

Context *VKBackend::context_alloc(void *ghost_window, void *ghost_context)
{
  if (ghost_window) {
    BLI_assert(ghost_context == nullptr);
    ghost_context = GHOST_GetDrawingContext((GHOST_WindowHandle)ghost_window);
  }

  BLI_assert(ghost_context != nullptr);
  if (!device.is_initialized()) {
    device.init(ghost_context);
  }

  VKContext *context = new VKContext(ghost_window, ghost_context, device.current_thread_data());
  device.context_register(*context);
  GHOST_SetVulkanSwapBuffersCallbacks((GHOST_ContextHandle)ghost_context,
                                      VKContext::swap_buffers_pre_callback,
                                      VKContext::swap_buffers_post_callback);
  return context;
}

Batch *VKBackend::batch_alloc()
{
  return new VKBatch();
}

DrawList *VKBackend::drawlist_alloc(int list_length)
{
  return new VKDrawList(list_length);
}

Fence *VKBackend::fence_alloc()
{
  return new VKFence();
}

FrameBuffer *VKBackend::framebuffer_alloc(const char *name)
{
  return new VKFrameBuffer(name);
}

IndexBuf *VKBackend::indexbuf_alloc()
{
  return new VKIndexBuffer();
}

PixelBuffer *VKBackend::pixelbuf_alloc(size_t size)
{
  return new VKPixelBuffer(size);
}

QueryPool *VKBackend::querypool_alloc()
{
  return new VKQueryPool();
}

Shader *VKBackend::shader_alloc(const char *name)
{
  return new VKShader(name);
}

Texture *VKBackend::texture_alloc(const char *name)
{
  return new VKTexture(name);
}

UniformBuf *VKBackend::uniformbuf_alloc(size_t size, const char *name)
{
  return new VKUniformBuffer(size, name);
}

StorageBuf *VKBackend::storagebuf_alloc(size_t size, GPUUsageType usage, const char *name)
{
  return new VKStorageBuffer(size, usage, name);
}

VertBuf *VKBackend::vertbuf_alloc()
{
  return new VKVertexBuffer();
}

void VKBackend::render_begin()
{
  VKThreadData &thread_data = device.current_thread_data();
  BLI_assert_msg(thread_data.rendering_depth >= 0, "Unbalanced `GPU_render_begin/end`");
  thread_data.rendering_depth += 1;
}

void VKBackend::render_end()
{
  VKThreadData &thread_data = device.current_thread_data();
  thread_data.rendering_depth -= 1;
  BLI_assert_msg(thread_data.rendering_depth >= 0, "Unbalanced `GPU_render_begin/end`");

  if (G.background) {
    if (thread_data.rendering_depth == 0) {
      thread_data.resource_pool_next();

      VKResourcePool &resource_pool = thread_data.resource_pool_get();
      resource_pool.discard_pool.destroy_discarded_resources(device);
      resource_pool.reset();
      resource_pool.discard_pool.move_data(device.orphaned_data);
    }
  }
}

void VKBackend::render_step() {}

void VKBackend::capabilities_init(VKDevice &device)
{
  const VkPhysicalDeviceProperties &properties = device.physical_device_properties_get();
  const VkPhysicalDeviceLimits &limits = properties.limits;

  /* Reset all capabilities from previous context. */
  GCaps = {};
  GCaps.geometry_shader_support = true;
  GCaps.texture_view_support = true;
  GCaps.stencil_export_support = device.supports_extension(
      VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
  GCaps.shader_draw_parameters_support =
      device.physical_device_vulkan_11_features_get().shaderDrawParameters;

  GCaps.max_texture_size = max_ii(limits.maxImageDimension1D, limits.maxImageDimension2D);
  GCaps.max_texture_3d_size = limits.maxImageDimension3D;
  GCaps.max_texture_layers = limits.maxImageArrayLayers;
  GCaps.max_textures = limits.maxDescriptorSetSampledImages;
  GCaps.max_textures_vert = limits.maxPerStageDescriptorSampledImages;
  GCaps.max_textures_geom = limits.maxPerStageDescriptorSampledImages;
  GCaps.max_textures_frag = limits.maxPerStageDescriptorSampledImages;
  GCaps.max_samplers = limits.maxSamplerAllocationCount;
  GCaps.max_images = limits.maxPerStageDescriptorStorageImages;
  for (int i = 0; i < 3; i++) {
    GCaps.max_work_group_count[i] = limits.maxComputeWorkGroupCount[i];
    GCaps.max_work_group_size[i] = limits.maxComputeWorkGroupSize[i];
  }
  GCaps.max_uniforms_vert = limits.maxPerStageDescriptorUniformBuffers;
  GCaps.max_uniforms_frag = limits.maxPerStageDescriptorUniformBuffers;
  GCaps.max_batch_indices = limits.maxDrawIndirectCount;
  GCaps.max_batch_vertices = limits.maxDrawIndexedIndexValue;
  GCaps.max_vertex_attribs = limits.maxVertexInputAttributes;
  GCaps.max_varying_floats = limits.maxVertexOutputComponents;
  GCaps.max_shader_storage_buffer_bindings = limits.maxPerStageDescriptorStorageBuffers;
  GCaps.max_compute_shader_storage_blocks = limits.maxPerStageDescriptorStorageBuffers;
  GCaps.max_storage_buffer_size = size_t(limits.maxStorageBufferRange);

  GCaps.max_parallel_compilations = BLI_system_thread_count();
  GCaps.mem_stats_support = true;

  detect_workarounds(device);
}

}  // namespace blender::gpu
