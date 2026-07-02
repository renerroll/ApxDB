#include "apxdb.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#include "vulkan_shader_spirv.h"
#endif

typedef struct {
  char* id;
  uint8_t* data;
  size_t length;
  bool is_json;
} apxdb_document_t;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;
static apxdb_state_t g_state = APXDB_STATE_CLOSED;
static bool g_gpu_available = false;
static apxdb_gpu_status_t g_gpu_status = APXDB_GPU_UNAVAILABLE;
char* g_storage_directory = NULL;
static apxdb_document_t* g_documents = NULL;
static size_t g_document_count = 0;
static size_t g_document_capacity = 0;
static atomic_int_fast32_t g_document_counter = ATOMIC_VAR_INIT(0);

static char* allocate_utf8_string(const char* text) {
  if (!text) {
    return NULL;
  }
  size_t length = strlen(text);
  char* buffer = (char*)malloc(length + 1);
  if (!buffer) {
    return NULL;
  }
  memcpy(buffer, text, length + 1);
  return buffer;
}

static bool push_document(const char* id, const void* data, size_t length, bool is_json) {
  if (g_document_count >= g_document_capacity) {
    size_t new_capacity = g_document_capacity == 0 ? 8 : g_document_capacity * 2;
    apxdb_document_t* next = (apxdb_document_t*)realloc(g_documents, new_capacity * sizeof(apxdb_document_t));
    if (!next) {
      return false;
    }
    g_documents = next;
    g_document_capacity = new_capacity;
  }

  g_documents[g_document_count].id = allocate_utf8_string(id);
  g_documents[g_document_count].length = length;
  g_documents[g_document_count].is_json = is_json;
  g_documents[g_document_count].data = (uint8_t*)malloc(length + (is_json ? 1 : 0));
  if (!g_documents[g_document_count].id || !g_documents[g_document_count].data) {
    free(g_documents[g_document_count].id);
    free(g_documents[g_document_count].data);
    return false;
  }
  memcpy(g_documents[g_document_count].data, data, length);
  if (is_json) {
    g_documents[g_document_count].data[length] = '\0';
  }

  g_document_count += 1;
  return true;
}

static void free_documents(void) {
  for (size_t index = 0; index < g_document_count; ++index) {
    free(g_documents[index].id);
    free(g_documents[index].data);
  }
  free(g_documents);
  g_documents = NULL;
  g_document_count = 0;
  g_document_capacity = 0;
}

static bool ensure_directory_exists_native(const char* path) {
  if (!path) {
    return false;
  }
  struct stat st;
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  if (errno != ENOENT) {
    return false;
  }
  return mkdir(path, 0755) == 0;
}

static void init_gpu_backend(void);
static void transition_to_closed(void);
static void cleanup_partial_open(void);
static void set_state(apxdb_state_t state);static void set_gpu_status(apxdb_gpu_status_t status);
static bool set_storage_directory(const char* directory_path) {
  if (!directory_path) {
    return false;
  }
  char* next = allocate_utf8_string(directory_path);
  if (!next) {
    return false;
  }
  free(g_storage_directory);
  g_storage_directory = next;
  return true;
}

static int32_t initialize_internal(void) {
  free_documents();
  atomic_store(&g_document_counter, 0);
  init_gpu_backend();
  return g_gpu_available ? APXDB_OK : APXDB_OK_GPU_FALLBACK;
}

int32_t apxdb_gpu_status(void) {
  pthread_mutex_lock(&g_mutex);
  int32_t status = (int32_t)g_gpu_status;
  pthread_mutex_unlock(&g_mutex);
  return status;
}

static bool g_gpu_force_disabled = false;
static void cleanup_gpu_backend(void);
static void set_state(apxdb_state_t state) {
  g_state = state;
  g_initialized = (state == APXDB_STATE_OPEN);
}

void apxdb_set_gpu_enabled(bool enabled) {
  pthread_mutex_lock(&g_mutex);
  g_gpu_force_disabled = !enabled;
  pthread_mutex_unlock(&g_mutex);
}

static void set_gpu_status(apxdb_gpu_status_t status) {
  g_gpu_status = status;
}

static void transition_to_closed(void) {
  if (g_storage_directory) {
    apxdb_save_all_collections(g_storage_directory);
  }
  cleanup_gpu_backend();
  free_documents();
  apxdb_collection_unregister_all();
  apxdb_schema_unregister_all();
  free(g_storage_directory);
  g_storage_directory = NULL;
  set_state(APXDB_STATE_CLOSED);
}

static void cleanup_partial_open(void) {
  cleanup_gpu_backend();
  free_documents();
  apxdb_collection_unregister_all();
  apxdb_schema_unregister_all();
  free(g_storage_directory);
  g_storage_directory = NULL;
  g_initialized = false;
}

#ifdef __ANDROID__
static VkInstance g_vk_instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_vk_physical_device = VK_NULL_HANDLE;
static VkDevice g_vk_device = VK_NULL_HANDLE;
static VkQueue g_vk_queue = VK_NULL_HANDLE;
static uint32_t g_vk_queue_family = UINT32_MAX;
static VkCommandPool g_vk_command_pool = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_vk_descriptor_set_layout = VK_NULL_HANDLE;
static VkPipelineLayout g_vk_pipeline_layout = VK_NULL_HANDLE;
static VkPipeline g_vk_pipeline = VK_NULL_HANDLE;
static VkDescriptorPool g_vk_descriptor_pool = VK_NULL_HANDLE;

static bool initialize_vulkan_backend(void);
static bool run_vulkan_query_int(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint8_t* out_mask);
static void cleanup_vulkan_backend(void);
#endif

#ifdef __OBJC__
static void cleanup_metal_backend(void);
static id<MTLDevice> g_metal_device = nil;
static id<MTLCommandQueue> g_metal_command_queue = nil;
static id<MTLLibrary> g_metal_library = nil;
static id<MTLComputePipelineState> g_metal_pipeline = nil;
static id<MTLComputePipelineState> g_metal_count_phase1_pipeline = nil;
static id<MTLComputePipelineState> g_metal_count_phase2_pipeline = nil;
static id<MTLBuffer> g_metal_input_buffer = nil;
static id<MTLBuffer> g_metal_valid_buffer = nil;
static id<MTLBuffer> g_metal_output_buffer = nil;
static id<MTLBuffer> g_metal_params_buffer = nil;
static id<MTLBuffer> g_metal_count_buffer = nil;
static id<MTLBuffer> g_metal_partial_counts_buffer = nil;

static const char kMetalShaderSource[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void documentCompute(device const int* input [[buffer(0)]],\n"
"                            device const uint* valid [[buffer(1)]],\n"
"                            device uint* output [[buffer(2)]],\n"
"                            device const int* params [[buffer(3)]],\n"
"                            uint tid [[thread_position_in_grid]]) {\n"
"  bool match = false;\n"
"  if (valid[tid] != 0u) {\n"
"    int op = params[0];\n"
"    int threshold = params[1];\n"
"    int value = input[tid];\n"
"    switch (op) {\n"
"      case 0: match = (value == threshold); break;\n"
"      case 1: match = (value > threshold); break;\n"
"      case 2: match = (value >= threshold); break;\n"
"      case 3: match = (value < threshold); break;\n"
"      case 4: match = (value <= threshold); break;\n"
"      default: match = false; break;\n"
"    }\n"
"  }\n"
"  output[tid] = match ? 1u : 0u;\n"
"}\n"
"kernel void documentCountPhase1(device const int* input [[buffer(0)]],\n"
"                                    device const uint* valid [[buffer(1)]],\n"
"                                    device const int* params [[buffer(2)]],\n"
"                                    device uint* partial_counts [[buffer(3)]],\n"
"                                    uint tid [[thread_position_in_grid]],\n"
"                                    uint thread_idx [[thread_index_in_threadgroup]], uint group_size [[threads_per_threadgroup]]) {\n"
"  uint group_id = tid / group_size;\n"
"  bool match = false;\n"
"  if (valid[tid] != 0u) {\n"
"    int op = params[0];\n"
"    int threshold = params[1];\n"
"    int value = input[tid];\n"
"    switch (op) {\n"
"      case 0: match = (value == threshold); break;\n"
"      case 1: match = (value > threshold); break;\n"
"      case 2: match = (value >= threshold); break;\n"
"      case 3: match = (value < threshold); break;\n"
"      case 4: match = (value <= threshold); break;\n"
"      default: match = false; break;\n"
"    }\n"
"  }\n"
"  threadgroup uint local_counts[1024];\n"
"  local_counts[thread_idx] = match ? 1u : 0u;\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  uint size = group_size;\n"
"  for (uint stride = size / 2; stride > 0; stride /= 2) {\n"
"    if (thread_idx < stride) {\n"
"      local_counts[thread_idx] += local_counts[thread_idx + stride];\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  }\n"
"  if (thread_idx == 0) {\n"
"    partial_counts[group_id] = local_counts[0];\n"
"  }\n"
"}\n"
"kernel void documentCountPhase2(device const uint* partial_counts [[buffer(0)]],\n"
"                                    device atomic_uint* count [[buffer(1)]],\n"
"                                    uint tid [[thread_position_in_grid]],\n"
"                                    uint thread_idx [[thread_index_in_threadgroup]], uint group_size [[threads_per_threadgroup]]) {\n"
"  threadgroup uint local_counts[1024];\n"
"  local_counts[thread_idx] = partial_counts[tid];\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  uint size = group_size;\n"
"  for (uint stride = size / 2; stride > 0; stride /= 2) {\n"
"    if (thread_idx < stride) {\n"
"      local_counts[thread_idx] += local_counts[thread_idx + stride];\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  }\n"
"  if (thread_idx == 0) {\n"
"    uint partial = local_counts[0];\n"
"    if (partial > 0u) {\n"
"      atomic_fetch_add_explicit(count, partial, memory_order_relaxed);\n"
"    }\n"
"  }\n"
"}\n";
static bool initialize_metal_backend(void) {
  g_metal_device = MTLCreateSystemDefaultDevice();
  if (!g_metal_device) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal device unavailable\n");
#endif
    return false;
  }
  g_metal_command_queue = [g_metal_device newCommandQueue];
  if (!g_metal_command_queue) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal command queue creation failed\n");
#endif
    return false;
  }
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kMetalShaderSource];
  g_metal_library = [g_metal_device newLibraryWithSource:source options:nil error:&error];
  if (!g_metal_library) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal library build failed: %s\n", [[error localizedDescription] UTF8String]);
#endif
    cleanup_metal_backend();
    return false;
  }
  id<MTLFunction> function = [g_metal_library newFunctionWithName:@"documentCompute"];
  if (!function) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal function lookup failed\n");
#endif
    cleanup_metal_backend();
    return false;
  }
  g_metal_pipeline = [g_metal_device newComputePipelineStateWithFunction:function error:&error];
  if (!g_metal_pipeline) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal pipeline creation failed: %s\n", [[error localizedDescription] UTF8String]);
#endif
    cleanup_metal_backend();
    return false;
  }

  id<MTLFunction> countPhase1Function = [g_metal_library newFunctionWithName:@"documentCountPhase1"];
  if (!countPhase1Function) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal count phase1 function lookup failed\n");
#endif
    cleanup_metal_backend();
    return false;
  }
  g_metal_count_phase1_pipeline = [g_metal_device newComputePipelineStateWithFunction:countPhase1Function error:&error];
  if (!g_metal_count_phase1_pipeline) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal count phase1 pipeline creation failed: %s\n", [[error localizedDescription] UTF8String]);
#endif
    cleanup_metal_backend();
    return false;
  }

  id<MTLFunction> countPhase2Function = [g_metal_library newFunctionWithName:@"documentCountPhase2"];
  if (!countPhase2Function) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal count phase2 function lookup failed\n");
#endif
    cleanup_metal_backend();
    return false;
  }
  g_metal_count_phase2_pipeline = [g_metal_device newComputePipelineStateWithFunction:countPhase2Function error:&error];
  if (!g_metal_count_phase2_pipeline) {
#if defined(APXDB_ENABLE_DIAGNOSTICS)
    fprintf(stderr, "apxdb: Metal count phase2 pipeline creation failed: %s\n", [[error localizedDescription] UTF8String]);
#endif
    cleanup_metal_backend();
    return false;
  }
  return true;
}

static id<MTLBuffer> ensure_metal_buffer(id<MTLBuffer> buffer, size_t size) {
  if (!buffer || [buffer length] < size) {
    buffer = [g_metal_device newBufferWithLength:size options:MTLResourceStorageModeShared];
  }
  return buffer;
}

#ifdef __OBJC__
static NSUInteger metal_threadgroup_size(NSUInteger width, NSUInteger maxThreads, NSUInteger count, const char* env_name) {
  NSUInteger threadGroupSize = width * 4;
  const char* env_value = getenv(env_name);
  if (env_value) {
    unsigned long long override_value = strtoull(env_value, NULL, 10);
    if (override_value > 0) {
      threadGroupSize = (NSUInteger)override_value;
    }
  }
  if (threadGroupSize > maxThreads) {
    threadGroupSize = maxThreads;
  }
  if (threadGroupSize > count) {
    threadGroupSize = count;
  }
  if (width > 0) {
    threadGroupSize = (threadGroupSize / width) * width;
    if (threadGroupSize == 0) {
      threadGroupSize = width;
    }
  }
  if (threadGroupSize == 0) {
    threadGroupSize = 1;
  }
  return threadGroupSize;
}
#endif

static void cleanup_metal_backend(void) {
#ifdef __OBJC__
  g_metal_count_buffer = nil;
  g_metal_partial_counts_buffer = nil;
  g_metal_params_buffer = nil;
  g_metal_output_buffer = nil;
  g_metal_valid_buffer = nil;
  g_metal_input_buffer = nil;
  g_metal_pipeline = nil;
  g_metal_count_phase1_pipeline = nil;
  g_metal_count_phase2_pipeline = nil;
  g_metal_library = nil;
  g_metal_command_queue = nil;
  g_metal_device = nil;
#endif
}
#endif

#ifdef __ANDROID__
static uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memory_properties;
  vkGetPhysicalDeviceMemoryProperties(g_vk_physical_device, &memory_properties);
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    if ((type_filter & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  return UINT32_MAX;
}

static bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer, VkDeviceMemory* memory) {
  VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(g_vk_device, &buffer_info, NULL, buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements mem_requirements;
  vkGetBufferMemoryRequirements(g_vk_device, *buffer, &mem_requirements);

  uint32_t memory_type = find_memory_type(mem_requirements.memoryTypeBits, properties);
  if (memory_type == UINT32_MAX) {
    vkDestroyBuffer(g_vk_device, *buffer, NULL);
    return false;
  }

  VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc_info.allocationSize = mem_requirements.size;
  alloc_info.memoryTypeIndex = memory_type;

  if (vkAllocateMemory(g_vk_device, &alloc_info, NULL, memory) != VK_SUCCESS) {
    vkDestroyBuffer(g_vk_device, *buffer, NULL);
    return false;
  }

  if (vkBindBufferMemory(g_vk_device, *buffer, *memory, 0) != VK_SUCCESS) {
    vkDestroyBuffer(g_vk_device, *buffer, NULL);
    vkFreeMemory(g_vk_device, *memory, NULL);
    return false;
  }

  return true;
}

static bool initialize_vulkan_backend(void) {
  VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "ApxDB";
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "ApxDBEngine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app_info;
  if (vkCreateInstance(&instance_info, NULL, &g_vk_instance) != VK_SUCCESS) {
    cleanup_vulkan_backend();
    return false;
  }

  uint32_t physical_device_count = 0;
  if (vkEnumeratePhysicalDevices(g_vk_instance, &physical_device_count, NULL) != VK_SUCCESS || physical_device_count == 0) {
    return false;
  }

  VkPhysicalDevice physical_devices[8];
  if (physical_device_count > 8) {
    physical_device_count = 8;
  }
  if (vkEnumeratePhysicalDevices(g_vk_instance, &physical_device_count, physical_devices) != VK_SUCCESS) {
    cleanup_vulkan_backend();
    return false;
  }

  for (uint32_t i = 0; i < physical_device_count; ++i) {
    VkPhysicalDeviceProperties physical_properties;
    vkGetPhysicalDeviceProperties(physical_devices[i], &physical_properties);

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &queue_family_count, NULL);
    if (queue_family_count == 0) {
      continue;
    }

    VkQueueFamilyProperties queue_families[16];
    if (queue_family_count > 16) {
      queue_family_count = 16;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &queue_family_count, queue_families);

    for (uint32_t j = 0; j < queue_family_count; ++j) {
      if (queue_families[j].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        g_vk_physical_device = physical_devices[i];
        g_vk_queue_family = j;
        break;
      }
    }
    if (g_vk_physical_device != VK_NULL_HANDLE) {
      break;
    }
  }

  if (g_vk_physical_device == VK_NULL_HANDLE || g_vk_queue_family == UINT32_MAX) {
    cleanup_vulkan_backend();
    return false;
  }

  float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo queue_create_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_create_info.queueFamilyIndex = g_vk_queue_family;
  queue_create_info.queueCount = 1;
  queue_create_info.pQueuePriorities = &queue_priority;

  VkDeviceCreateInfo device_create_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_create_info.queueCreateInfoCount = 1;
  device_create_info.pQueueCreateInfos = &queue_create_info;

  if (vkCreateDevice(g_vk_physical_device, &device_create_info, NULL, &g_vk_device) != VK_SUCCESS) {
    cleanup_vulkan_backend();
    return false;
  }

  vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);

  VkCommandPoolCreateInfo command_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  command_pool_info.queueFamilyIndex = g_vk_queue_family;
  command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if (vkCreateCommandPool(g_vk_device, &command_pool_info, NULL, &g_vk_command_pool) != VK_SUCCESS) {
    cleanup_vulkan_backend();
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[4];
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[0].pImmutableSamplers = NULL;

  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1].pImmutableSamplers = NULL;

  bindings[2].binding = 2;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[2].pImmutableSamplers = NULL;

  bindings[3].binding = 3;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[3].descriptorCount = 1;
  bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[3].pImmutableSamplers = NULL;

  VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptor_layout_info.bindingCount = 4;
  descriptor_layout_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(g_vk_device, &descriptor_layout_info, NULL, &g_vk_descriptor_set_layout) != VK_SUCCESS) {
    return false;
  }

  VkPipelineLayoutCreateInfo pipeline_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &g_vk_descriptor_set_layout;
  if (vkCreatePipelineLayout(g_vk_device, &pipeline_layout_info, NULL, &g_vk_pipeline_layout) != VK_SUCCESS) {
    return false;
  }

  VkShaderModuleCreateInfo shader_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader_info.codeSize = kVulkanShaderSpirvSize;
  shader_info.pCode = (const uint32_t*)kVulkanShaderSpirv;
  VkShaderModule shader_module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(g_vk_device, &shader_info, NULL, &shader_module) != VK_SUCCESS) {
    return false;
  }

  VkPipelineShaderStageCreateInfo shader_stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  shader_stage.module = shader_module;
  shader_stage.pName = "main";

  VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = shader_stage;
  pipeline_info.layout = g_vk_pipeline_layout;
  if (vkCreateComputePipelines(g_vk_device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &g_vk_pipeline) != VK_SUCCESS) {
    vkDestroyShaderModule(g_vk_device, shader_module, NULL);
    return false;
  }
  vkDestroyShaderModule(g_vk_device, shader_module, NULL);

  VkDescriptorPoolSize pool_sizes[4];
  for (uint32_t i = 0; i < 4; ++i) {
    pool_sizes[i].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[i].descriptorCount = 1;
  }

  VkDescriptorPoolCreateInfo descriptor_pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  descriptor_pool_info.poolSizeCount = 4;
  descriptor_pool_info.pPoolSizes = pool_sizes;
  descriptor_pool_info.maxSets = 1;
  if (vkCreateDescriptorPool(g_vk_device, &descriptor_pool_info, NULL, &g_vk_descriptor_pool) != VK_SUCCESS) {
    cleanup_vulkan_backend();
    return false;
  }

  return true;
}

static void cleanup_vulkan_backend(void) {
  // Правильний порядок destroy:
  // 1. vkFreeCommandBuffers()        - free outstanding command buffers
  // 2. vkDestroyCommandPool()         - destroy the pool after buffers are freed
  // 3. vkDestroyPipeline()            - destroy compute pipeline
  // 4. vkDestroyPipelineLayout()      - destroy pipeline layout
  // 5. vkDestroyDescriptorSetLayout() - destroy descriptor set layout
  // 6. vkDestroyDescriptorPool()      - destroy descriptor pool
  // 7. vkDestroyShaderModule()        - destroy shader module if retained
  // 8. vkFreeMemory() for all buffers - free any buffer memory
  // 9. vkDestroyBuffer()              - destroy any buffers
  // 10. vkDestroyDevice()             - destroy the logical device
  // 11. vkDestroyInstance()           - destroy the Vulkan instance last
  // Note: buffer and command buffer cleanup is handled per-query in run_vulkan_query_int().

  if (g_vk_device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(g_vk_device);
  }
  if (g_vk_command_pool != VK_NULL_HANDLE) {
    vkDestroyCommandPool(g_vk_device, g_vk_command_pool, NULL);
    g_vk_command_pool = VK_NULL_HANDLE;
  }
  if (g_vk_pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(g_vk_device, g_vk_pipeline, NULL);
    g_vk_pipeline = VK_NULL_HANDLE;
  }
  if (g_vk_pipeline_layout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(g_vk_device, g_vk_pipeline_layout, NULL);
    g_vk_pipeline_layout = VK_NULL_HANDLE;
  }
  if (g_vk_descriptor_set_layout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(g_vk_device, g_vk_descriptor_set_layout, NULL);
    g_vk_descriptor_set_layout = VK_NULL_HANDLE;
  }
  if (g_vk_descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(g_vk_device, g_vk_descriptor_pool, NULL);
    g_vk_descriptor_pool = VK_NULL_HANDLE;
  }
  if (g_vk_device != VK_NULL_HANDLE) {
    vkDestroyDevice(g_vk_device, NULL);
    g_vk_device = VK_NULL_HANDLE;
  }
  if (g_vk_instance != VK_NULL_HANDLE) {
    vkDestroyInstance(g_vk_instance, NULL);
    g_vk_instance = VK_NULL_HANDLE;
  }
  g_vk_physical_device = VK_NULL_HANDLE;
  g_vk_queue = VK_NULL_HANDLE;
  g_vk_queue_family = UINT32_MAX;
}

static bool run_vulkan_query_int(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint8_t* out_mask) {
  if (!values || !valid_mask || !out_mask || count == 0 || g_vk_device == VK_NULL_HANDLE || g_vk_queue == VK_NULL_HANDLE) {
    return false;
  }

  VkBuffer input_buffer = VK_NULL_HANDLE;
  VkDeviceMemory input_memory = VK_NULL_HANDLE;
  VkBuffer valid_buffer = VK_NULL_HANDLE;
  VkDeviceMemory valid_memory = VK_NULL_HANDLE;
  VkBuffer output_buffer = VK_NULL_HANDLE;
  VkDeviceMemory output_memory = VK_NULL_HANDLE;
  VkBuffer params_buffer = VK_NULL_HANDLE;
  VkDeviceMemory params_memory = VK_NULL_HANDLE;

  if (!create_buffer(count * sizeof(int32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &input_buffer, &input_memory)) {
    return false;
  }
  if (!create_buffer(count * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &valid_buffer, &valid_memory)) {
    vkDestroyBuffer(g_vk_device, input_buffer, NULL);
    vkFreeMemory(g_vk_device, input_memory, NULL);
    return false;
  }
  if (!create_buffer(count * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &output_buffer, &output_memory)) {
    vkDestroyBuffer(g_vk_device, input_buffer, NULL);
    vkFreeMemory(g_vk_device, input_memory, NULL);
    vkDestroyBuffer(g_vk_device, valid_buffer, NULL);
    vkFreeMemory(g_vk_device, valid_memory, NULL);
    return false;
  }
  if (!create_buffer(2 * sizeof(int32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &params_buffer, &params_memory)) {
    vkDestroyBuffer(g_vk_device, input_buffer, NULL);
    vkFreeMemory(g_vk_device, input_memory, NULL);
    vkDestroyBuffer(g_vk_device, valid_buffer, NULL);
    vkFreeMemory(g_vk_device, valid_memory, NULL);
    vkDestroyBuffer(g_vk_device, output_buffer, NULL);
    vkFreeMemory(g_vk_device, output_memory, NULL);
    return false;
  }

  void* mapped = NULL;
  vkMapMemory(g_vk_device, input_memory, 0, count * sizeof(int32_t), 0, &mapped);
  memcpy(mapped, values, count * sizeof(int32_t));
  vkUnmapMemory(g_vk_device, input_memory);

  vkMapMemory(g_vk_device, valid_memory, 0, count * sizeof(uint32_t), 0, &mapped);
  memcpy(mapped, valid_mask, count * sizeof(uint32_t));
  vkUnmapMemory(g_vk_device, valid_memory);

  vkMapMemory(g_vk_device, params_memory, 0, 2 * sizeof(int32_t), 0, &mapped);
  int32_t params[2] = {op, threshold};
  memcpy(mapped, params, sizeof(params));
  vkUnmapMemory(g_vk_device, params_memory);

  VkDescriptorSetAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = g_vk_descriptor_pool;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &g_vk_descriptor_set_layout;

  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(g_vk_device, &alloc_info, &descriptor_set) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorBufferInfo input_info = {input_buffer, 0, count * sizeof(int32_t)};
  VkDescriptorBufferInfo valid_info = {valid_buffer, 0, count * sizeof(uint32_t)};
  VkDescriptorBufferInfo output_info = {output_buffer, 0, count * sizeof(uint32_t)};
  VkDescriptorBufferInfo params_info = {params_buffer, 0, 2 * sizeof(int32_t)};

  VkWriteDescriptorSet writes[4];
  writes[0] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[0].dstSet = descriptor_set;
  writes[0].dstBinding = 0;
  writes[0].dstArrayElement = 0;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &input_info;

  writes[1] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[1].dstSet = descriptor_set;
  writes[1].dstBinding = 1;
  writes[1].dstArrayElement = 0;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].descriptorCount = 1;
  writes[1].pBufferInfo = &valid_info;

  writes[2] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[2].dstSet = descriptor_set;
  writes[2].dstBinding = 2;
  writes[2].dstArrayElement = 0;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].descriptorCount = 1;
  writes[2].pBufferInfo = &output_info;

  writes[3] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[3].dstSet = descriptor_set;
  writes[3].dstBinding = 3;
  writes[3].dstArrayElement = 0;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].descriptorCount = 1;
  writes[3].pBufferInfo = &params_info;

  vkUpdateDescriptorSets(g_vk_device, 4, writes, 0, NULL);

  VkCommandBufferAllocateInfo command_buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_buffer_info.commandPool = g_vk_command_pool;
  command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_info.commandBufferCount = 1;

  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(g_vk_device, &command_buffer_info, &command_buffer) != VK_SUCCESS) {
    return false;
  }

  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_pipeline_layout, 0, 1, &descriptor_set, 0, NULL);

  uint32_t work_groups = (uint32_t)((count + 63) / 64);
  vkCmdDispatch(command_buffer, work_groups, 1, 1);
  vkEndCommandBuffer(command_buffer);

  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(g_vk_device, &fence_info, NULL, &fence) != VK_SUCCESS) {
    return false;
  }

  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  if (vkQueueSubmit(g_vk_queue, 1, &submit_info, fence) != VK_SUCCESS) {
    vkDestroyFence(g_vk_device, fence, NULL);
    return false;
  }

  vkWaitForFences(g_vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(g_vk_device, fence, NULL);
  vkFreeCommandBuffers(g_vk_device, g_vk_command_pool, 1, &command_buffer);

  vkMapMemory(g_vk_device, output_memory, 0, count * sizeof(uint32_t), 0, &mapped);
  uint32_t* results = (uint32_t*)mapped;
  for (size_t i = 0; i < count; ++i) {
    out_mask[i] = results[i] ? 1 : 0;
  }
  vkUnmapMemory(g_vk_device, output_memory);

  vkDestroyBuffer(g_vk_device, input_buffer, NULL);
  vkFreeMemory(g_vk_device, input_memory, NULL);
  vkDestroyBuffer(g_vk_device, valid_buffer, NULL);
  vkFreeMemory(g_vk_device, valid_memory, NULL);
  vkDestroyBuffer(g_vk_device, params_buffer, NULL);
  vkFreeMemory(g_vk_device, params_memory, NULL);
  vkDestroyBuffer(g_vk_device, output_buffer, NULL);
  vkFreeMemory(g_vk_device, output_memory, NULL);

  vkResetDescriptorPool(g_vk_device, g_vk_descriptor_pool, 0);
  return true;
}
#endif

bool run_gpu_query_int(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint8_t* out_mask) {
  if (!g_gpu_available) {
    return false;
  }
#ifdef __OBJC__
  if (!values || !valid_mask || !out_mask || count == 0 || !g_metal_pipeline || !g_metal_command_queue) {
    return false;
  }

  size_t input_size = count * sizeof(int32_t);
  size_t valid_size = count * sizeof(uint32_t);
  size_t output_size = count * sizeof(uint32_t);

  g_metal_input_buffer = ensure_metal_buffer(g_metal_input_buffer, input_size);
  g_metal_valid_buffer = ensure_metal_buffer(g_metal_valid_buffer, valid_size);
  g_metal_output_buffer = ensure_metal_buffer(g_metal_output_buffer, output_size);
  if (!g_metal_input_buffer || !g_metal_valid_buffer || !g_metal_output_buffer) {
    return false;
  }

  memcpy([g_metal_input_buffer contents], values, input_size);
  [g_metal_input_buffer didModifyRange:NSMakeRange(0, input_size)];
  memcpy([g_metal_valid_buffer contents], valid_mask, valid_size);
  [g_metal_valid_buffer didModifyRange:NSMakeRange(0, valid_size)];
  int32_t params[2] = {op, threshold};
  if (!g_metal_params_buffer) {
    g_metal_params_buffer = [g_metal_device newBufferWithLength:sizeof(params) options:MTLResourceStorageModeShared];
    if (!g_metal_params_buffer) {
      return false;
    }
  }
  memcpy([g_metal_params_buffer contents], params, sizeof(params));
  [g_metal_params_buffer didModifyRange:NSMakeRange(0, sizeof(params))];

  id<MTLCommandBuffer> commandBuffer = [g_metal_command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
  [encoder setComputePipelineState:g_metal_pipeline];
  [encoder setBuffer:g_metal_input_buffer offset:0 atIndex:0];
  [encoder setBuffer:g_metal_valid_buffer offset:0 atIndex:1];
  [encoder setBuffer:g_metal_output_buffer offset:0 atIndex:2];
  [encoder setBuffer:g_metal_params_buffer offset:0 atIndex:3];

  MTLSize gridSize = MTLSizeMake(count, 1, 1);
  NSUInteger width = g_metal_pipeline.threadExecutionWidth;
  NSUInteger maxThreads = g_metal_pipeline.maxTotalThreadsPerThreadgroup;
  NSUInteger threadGroupSize = metal_threadgroup_size(width, maxThreads, count, "APXDB_METAL_THREADGROUP_SIZE");
#if defined(APXDB_ENABLE_DIAGNOSTICS)
  fprintf(stderr, "apxdb: gpu dispatch count=%zu threadExecutionWidth=%lu threadgroup=%lu maxPerGroup=%lu env=%s\n",
          count,
          (unsigned long)width,
          (unsigned long)threadGroupSize,
          (unsigned long)maxThreads,
          getenv("APXDB_METAL_THREADGROUP_SIZE") ?: "default");
#endif
  MTLSize threadgroupSize = MTLSizeMake(threadGroupSize, 1, 1);
  [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
  [encoder endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  uint32_t* gpu_results = (uint32_t*)[g_metal_output_buffer contents];
  for (size_t i = 0; i < count; ++i) {
    out_mask[i] = gpu_results[i] ? 1 : 0;
  }
  return true;
#elif defined(__ANDROID__)
  return run_vulkan_query_int(values, valid_mask, count, op, threshold, out_mask);
#else
  (void)values;
  (void)count;
  (void)op;
  (void)threshold;
  (void)out_mask;
  return false;
#endif
}

bool run_gpu_query_int_count(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint32_t* out_count) {
  if (!g_gpu_available) {
    return false;
  }
#ifdef __OBJC__
  if (!values || !valid_mask || !out_count || count == 0 || !g_metal_count_phase1_pipeline || !g_metal_count_phase2_pipeline || !g_metal_command_queue) {
    return false;
  }

  size_t input_size = count * sizeof(int32_t);
  size_t valid_size = count * sizeof(uint32_t);

  g_metal_input_buffer = ensure_metal_buffer(g_metal_input_buffer, input_size);
  g_metal_valid_buffer = ensure_metal_buffer(g_metal_valid_buffer, valid_size);
  g_metal_count_buffer = ensure_metal_buffer(g_metal_count_buffer, sizeof(uint32_t));
  if (!g_metal_input_buffer || !g_metal_valid_buffer || !g_metal_count_buffer) {
    return false;
  }

  memcpy([g_metal_input_buffer contents], values, input_size);
  [g_metal_input_buffer didModifyRange:NSMakeRange(0, input_size)];
  memcpy([g_metal_valid_buffer contents], valid_mask, valid_size);
  [g_metal_valid_buffer didModifyRange:NSMakeRange(0, valid_size)];
  uint32_t initial_count = 0;
  memcpy([g_metal_count_buffer contents], &initial_count, sizeof(initial_count));
  [g_metal_count_buffer didModifyRange:NSMakeRange(0, sizeof(initial_count))];

  int32_t params[2] = {op, threshold};
  if (!g_metal_params_buffer) {
    g_metal_params_buffer = [g_metal_device newBufferWithLength:sizeof(params) options:MTLResourceStorageModeShared];
    if (!g_metal_params_buffer) {
      return false;
    }
  }
  memcpy([g_metal_params_buffer contents], params, sizeof(params));
  [g_metal_params_buffer didModifyRange:NSMakeRange(0, sizeof(params))];

  NSUInteger width1 = g_metal_count_phase1_pipeline.threadExecutionWidth;
  NSUInteger maxThreads1 = g_metal_count_phase1_pipeline.maxTotalThreadsPerThreadgroup;
  NSUInteger threadGroupSize1 = metal_threadgroup_size(width1, maxThreads1, count, "APXDB_METAL_THREADGROUP1");

  size_t partial_count = (count + threadGroupSize1 - 1) / threadGroupSize1;
  size_t partial_size = partial_count * sizeof(uint32_t);
  g_metal_partial_counts_buffer = ensure_metal_buffer(g_metal_partial_counts_buffer, partial_size);
  if (!g_metal_partial_counts_buffer) {
    return false;
  }

  memset([g_metal_partial_counts_buffer contents], 0, partial_size);
  [g_metal_partial_counts_buffer didModifyRange:NSMakeRange(0, partial_size)];

  NSUInteger width2 = g_metal_count_phase2_pipeline.threadExecutionWidth;
  NSUInteger maxThreads2 = g_metal_count_phase2_pipeline.maxTotalThreadsPerThreadgroup;
  NSUInteger threadGroupSize2 = metal_threadgroup_size(width2, maxThreads2, partial_count, "APXDB_METAL_THREADGROUP2");

  id<MTLCommandBuffer> commandBuffer = [g_metal_command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];

  [encoder setComputePipelineState:g_metal_count_phase1_pipeline];
  [encoder setBuffer:g_metal_input_buffer offset:0 atIndex:0];
  [encoder setBuffer:g_metal_valid_buffer offset:0 atIndex:1];
  [encoder setBuffer:g_metal_params_buffer offset:0 atIndex:2];
  [encoder setBuffer:g_metal_partial_counts_buffer offset:0 atIndex:3];

  MTLSize gridSize1 = MTLSizeMake(count, 1, 1);
  MTLSize threadgroupSize1 = MTLSizeMake(threadGroupSize1, 1, 1);
  [encoder dispatchThreads:gridSize1 threadsPerThreadgroup:threadgroupSize1];

  [encoder setComputePipelineState:g_metal_count_phase2_pipeline];
  [encoder setBuffer:g_metal_partial_counts_buffer offset:0 atIndex:0];
  [encoder setBuffer:g_metal_count_buffer offset:0 atIndex:1];

  MTLSize gridSize2 = MTLSizeMake(partial_count, 1, 1);
  MTLSize threadgroupSize2 = MTLSizeMake(threadGroupSize2, 1, 1);
  [encoder dispatchThreads:gridSize2 threadsPerThreadgroup:threadgroupSize2];

  [encoder endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  uint32_t* gpu_count = (uint32_t*)[g_metal_count_buffer contents];
  *out_count = *gpu_count;
  return true;
#elif defined(__ANDROID__)
  (void)values;
  (void)valid_mask;
  (void)count;
  (void)op;
  (void)threshold;
  (void)out_count;
  return false;
#else
  (void)values;
  (void)valid_mask;
  (void)count;
  (void)op;
  (void)threshold;
  (void)out_count;
  return false;
#endif
}

static void init_gpu_backend(void) {
  if (g_gpu_force_disabled) {
    cleanup_gpu_backend();
    g_gpu_available = false;
    set_gpu_status(APXDB_GPU_UNAVAILABLE);
    return;
  }
#ifdef __APPLE__
#ifdef __OBJC__
  cleanup_gpu_backend();
  g_gpu_available = initialize_metal_backend();
  set_gpu_status(g_gpu_available ? APXDB_GPU_METAL_ACTIVE : APXDB_GPU_INIT_FAILED);
#if defined(APXDB_ENABLE_DIAGNOSTICS)
  fprintf(stderr, "apxdb: init_gpu_backend() status=%d\n", (int)g_gpu_status);
#endif
#else
  g_gpu_available = false;
  set_gpu_status(APXDB_GPU_UNAVAILABLE);
#endif
#elif defined(__ANDROID__)
  cleanup_gpu_backend();
  g_gpu_available = initialize_vulkan_backend();
  set_gpu_status(g_gpu_available ? APXDB_GPU_VULKAN_ACTIVE : APXDB_GPU_INIT_FAILED);
#else
  g_gpu_available = false;
  set_gpu_status(APXDB_GPU_UNAVAILABLE);
#endif
}

int32_t apxdb_initialize() {
  pthread_mutex_lock(&g_mutex);
  if (g_state == APXDB_STATE_OPEN) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_OK;
  }
  if (g_state == APXDB_STATE_OPENING || g_state == APXDB_STATE_CLOSING || g_state == APXDB_STATE_FAILED) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_PARTIAL_RECOVERY;
  }
  set_state(APXDB_STATE_OPENING);

  int32_t result = initialize_internal();
  set_state(APXDB_STATE_OPEN);
  pthread_mutex_unlock(&g_mutex);
  return result;
}

int32_t apxdb_shutdown() {
  pthread_mutex_lock(&g_mutex);
  if (g_state == APXDB_STATE_CLOSED) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_OK;
  }
  if (g_state == APXDB_STATE_OPENING || g_state == APXDB_STATE_CLOSING) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_PARTIAL_RECOVERY;
  }

  transition_to_closed();
  pthread_mutex_unlock(&g_mutex);
  return APXDB_OK;
}

int32_t apxdb_open(const char* directory_path) {
  if (!directory_path) {
    return APXDB_ERR_INVALID_ARGUMENT;
  }

  pthread_mutex_lock(&g_mutex);
  if (g_state == APXDB_STATE_OPEN) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_ALREADY_OPEN;
  }
  if (g_state == APXDB_STATE_OPENING || g_state == APXDB_STATE_CLOSING || g_state == APXDB_STATE_FAILED) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_PARTIAL_RECOVERY;
  }

  set_state(APXDB_STATE_OPENING);
  if (!ensure_directory_exists_native(directory_path)) {
    set_state(APXDB_STATE_FAILED);
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_IO;
  }

  int32_t result = initialize_internal();
  if (!set_storage_directory(directory_path)) {
    cleanup_partial_open();
    set_state(APXDB_STATE_FAILED);
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_UNKNOWN;
  }

  set_state(APXDB_STATE_OPEN);
  pthread_mutex_unlock(&g_mutex);
  return result;
}

int32_t apxdb_close(void) {
  pthread_mutex_lock(&g_mutex);
  if (g_state == APXDB_STATE_CLOSED) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_NOT_OPEN;
  }
  if (g_state == APXDB_STATE_OPENING || g_state == APXDB_STATE_CLOSING) {
    pthread_mutex_unlock(&g_mutex);
    return APXDB_ERR_PARTIAL_RECOVERY;
  }

  transition_to_closed();
  pthread_mutex_unlock(&g_mutex);
  return APXDB_OK;
}

static void cleanup_gpu_backend(void) {
#ifdef __APPLE__
#ifdef __OBJC__
  cleanup_metal_backend();
#endif
#elif defined(__ANDROID__)
  cleanup_vulkan_backend();
#else
  (void)0;
#endif
  g_gpu_available = false;
  set_gpu_status(APXDB_GPU_UNAVAILABLE);
}

const char* apxdb_create_document(const char* json_utf8) {
  if (!json_utf8) {
    return allocate_utf8_string("error:invalid_input");
  }

  pthread_mutex_lock(&g_mutex);
  if (g_state != APXDB_STATE_OPEN) {
    pthread_mutex_unlock(&g_mutex);
    return allocate_utf8_string("error:uninitialized");
  }

  int32_t next_id = atomic_fetch_add(&g_document_counter, 1) + 1;
  char id_string[32];
  snprintf(id_string, sizeof(id_string), "doc_%d", next_id);

  if (!push_document(id_string, json_utf8, strlen(json_utf8), true)) {
    pthread_mutex_unlock(&g_mutex);
    return allocate_utf8_string("error:allocation_failed");
  }

  pthread_mutex_unlock(&g_mutex);
  return allocate_utf8_string(id_string);
}

const char* apxdb_create_document_bytes(const uint8_t* bytes, size_t length) {
  if (!bytes || length == 0) {
    return allocate_utf8_string("error:invalid_input");
  }

  pthread_mutex_lock(&g_mutex);
  if (g_state != APXDB_STATE_OPEN) {
    pthread_mutex_unlock(&g_mutex);
    return allocate_utf8_string("error:uninitialized");
  }

  int32_t next_id = atomic_fetch_add(&g_document_counter, 1) + 1;
  char id_string[32];
  snprintf(id_string, sizeof(id_string), "doc_%d", next_id);

  if (!push_document(id_string, bytes, length, false)) {
    pthread_mutex_unlock(&g_mutex);
    return allocate_utf8_string("error:allocation_failed");
  }

  pthread_mutex_unlock(&g_mutex);
  return allocate_utf8_string(id_string);
}

const uint8_t* apxdb_get_document_bytes(const char* id, size_t* out_length) {
  if (!id || !out_length) {
    return NULL;
  }

  pthread_mutex_lock(&g_mutex);
  if (g_state != APXDB_STATE_OPEN) {
    pthread_mutex_unlock(&g_mutex);
    return NULL;
  }

  const uint8_t* result_data = NULL;
  size_t result_length = 0;
  for (size_t index = 0; index < g_document_count; ++index) {
    if (strcmp(g_documents[index].id, id) == 0) {
      result_length = g_documents[index].length;
      result_data = (const uint8_t*)malloc(result_length);
      if (result_data) {
        memcpy((void*)result_data, g_documents[index].data, result_length);
      }
      break;
    }
  }

  pthread_mutex_unlock(&g_mutex);
  if (result_data) {
    *out_length = result_length;
  }
  return result_data;
}

void apxdb_release_bytes(const uint8_t* bytes) {
  free((void*)bytes);
}

const char* apxdb_find_document(const char* query_utf8) {
  if (!query_utf8) {
    return allocate_utf8_string("error:invalid_input");
  }

  pthread_mutex_lock(&g_mutex);
  if (g_state != APXDB_STATE_OPEN) {
    pthread_mutex_unlock(&g_mutex);
    return allocate_utf8_string("error:uninitialized");
  }

  const char* result = "not_found";
  for (size_t index = 0; index < g_document_count; ++index) {
    if (strcmp(g_documents[index].id, query_utf8) == 0) {
      if (g_documents[index].is_json) {
        result = (const char*)g_documents[index].data;
      } else {
        result = "binary_document";
      }
      break;
    }
    if (g_documents[index].is_json && strstr((const char*)g_documents[index].data, query_utf8) != NULL) {
      result = (const char*)g_documents[index].data;
      break;
    }
  }

  char* output = allocate_utf8_string(result);
  pthread_mutex_unlock(&g_mutex);
  return output;
}

void apxdb_release_string(const char* utf8) {
  free((void*)utf8);
}

#include "apxdb_schema.c"
#include "apxdb_collection.c"
