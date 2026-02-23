// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "libretro_state.h"
#include "libretro_options.h"
#include "libretro_input.h"
#include "libretro_installer.h"
#include "libretro_log.h"
#include "libretro_game_loader.h"

#include <app/functions.h>
#include <audio/impl/libretro_audio.h>
#include <config/functions.h>
#include <config/state.h>
#include <config/version.h>
#include <display/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <gxm/state.h>
#include <io/state.h>
#include <kernel/state.h>
#include <renderer/functions.h>
#include <renderer/gl/state.h>
#include <renderer/state.h>
#include <renderer/vulkan/state.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <glad/glad.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>

#ifdef _WIN32
#include <Windows.h>
#endif

LibretroState libretro;

LibretroInputState &libretro_input_state() {
    return libretro.input;
}

// Forward declarations for core options functions
static void read_core_option(const char *key, const char *default_value, std::function<void(const char *)> apply_func);
static void apply_config_changes();
static void read_core_options();
static void stop_render_thread();
static void request_guest_shutdown(EmuEnvState &emuenv, const char *reason);
static bool perform_core_reset();

static void lr_message(const std::string &msg, unsigned frames = 240) {
    if (!libretro.environ_cb)
        return;
    retro_message m{};
    m.msg = msg.c_str();
    m.frames = frames;
    libretro.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

static const char *get_core_option_value(const char *key, const char *fallback) {
    if (!libretro.environ_cb)
        return fallback;

    retro_variable variable{};
    variable.key = key;
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value)
        return variable.value;
    return fallback;
}

static int parse_int_option(const char *value, int fallback) {
    if (!value)
        return fallback;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value)
        return fallback;
    return static_cast<int>(parsed);
}

static float parse_float_option(const char *value, float fallback) {
    if (!value)
        return fallback;
    char *end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value)
        return fallback;
    return parsed;
}

static std::string normalize_option_token(const char *value) {
    std::string token = value ? value : "";
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return token;
}

static bool parse_enabled_option(const char *value, bool fallback) {
    const std::string token = normalize_option_token(value);
    if (token == "enabled" || token == "enable" || token == "true" || token == "on" || token == "1" || token == "yes")
        return true;
    if (token == "disabled" || token == "disable" || token == "false" || token == "off" || token == "0" || token == "no")
        return false;
    return fallback;
}

static std::string canonical_memory_mapping_option(const char *value) {
    std::string token = normalize_option_token(value);
    std::replace(token.begin(), token.end(), '_', '-');
    std::replace(token.begin(), token.end(), ' ', '-');

    if (token == "doublebuffer")
        token = "double-buffer";
    if (token == "externalhost")
        token = "external-host";
    if (token == "pagetable")
        token = "page-table";
    if (token == "nativebuffer")
        token = "native-buffer";

    if (token != "disabled" && token != "double-buffer" && token != "external-host"
        && token != "page-table" && token != "native-buffer")
        token = "double-buffer";
    return token;
}

static void libretro_log_init() {
    struct retro_log_callback log;
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        libretro.log_cb = log.log;
}

void lr_log(enum retro_log_level level, const char *fmt, ...) {
    if (!libretro.log_cb)
        return;
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    libretro.log_cb(level, "[Vita3K] %s", buf);
}

static std::atomic<uint64_t> g_lr_trace_seq{ 0 };
static std::mutex g_lr_trace_file_mutex;

#ifdef _WIN32
static PVOID g_lr_vectored_exception_handler_handle = nullptr;
#endif

static void lr_trace(const char *tag, const char *fmt = nullptr, ...) {
    char details[3072] = {};
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(details, sizeof(details), fmt, ap);
        va_end(ap);
    }

    const uint64_t seq = g_lr_trace_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    const char *safe_tag = tag ? tag : "(null)";

    if (details[0])
        lr_log(RETRO_LOG_INFO, "[TRACE %llu] %s | %s\n", static_cast<unsigned long long>(seq), safe_tag, details);
    else
        lr_log(RETRO_LOG_INFO, "[TRACE %llu] %s\n", static_cast<unsigned long long>(seq), safe_tag);

    if (libretro.save_dir.empty())
        return;

    try {
        const fs::path trace_path = fs_utils::utf8_to_path(libretro.save_dir) / "libretro_trace_uwp.log";
        std::lock_guard<std::mutex> lock(g_lr_trace_file_mutex);
        fs::create_directories(trace_path.parent_path());

        std::ofstream trace_stream(trace_path.string(), std::ios::out | std::ios::app);
        if (!trace_stream.is_open())
            return;

        if (details[0])
            trace_stream << seq << "|" << safe_tag << "|" << details << "\n";
        else
            trace_stream << seq << "|" << safe_tag << "\n";
        trace_stream.flush();
    } catch (...) {
    }
}

#ifdef _WIN32
static LONG CALLBACK lr_vectored_exception_handler(EXCEPTION_POINTERS *ep) {
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD *record = ep->ExceptionRecord;
    const CONTEXT *ctx = ep->ContextRecord;

    const unsigned long code = static_cast<unsigned long>(record->ExceptionCode);
    const unsigned long flags = static_cast<unsigned long>(record->ExceptionFlags);
    const unsigned long param_count = static_cast<unsigned long>(record->NumberParameters);
    const unsigned long long p0 = (param_count > 0) ? static_cast<unsigned long long>(record->ExceptionInformation[0]) : 0ULL;
    const unsigned long long p1 = (param_count > 1) ? static_cast<unsigned long long>(record->ExceptionInformation[1]) : 0ULL;

#if defined(_M_X64) || defined(__x86_64__)
    const unsigned long long rip = ctx ? static_cast<unsigned long long>(ctx->Rip) : 0ULL;
    const unsigned long long rsp = ctx ? static_cast<unsigned long long>(ctx->Rsp) : 0ULL;
#else
    const unsigned long long rip = ctx ? static_cast<unsigned long long>(ctx->Eip) : 0ULL;
    const unsigned long long rsp = ctx ? static_cast<unsigned long long>(ctx->Esp) : 0ULL;
#endif

    lr_trace("veh.exception",
        "code=0x%08lX flags=0x%08lX addr=%p rip=0x%llX rsp=0x%llX param_count=%lu p0=0x%llX p1=0x%llX",
        code,
        flags,
        record->ExceptionAddress,
        rip,
        rsp,
        param_count,
        p0,
        p1);

    return EXCEPTION_CONTINUE_SEARCH;
}

static void lr_install_vectored_exception_handler() {
    if (g_lr_vectored_exception_handler_handle)
        return;

    g_lr_vectored_exception_handler_handle = AddVectoredExceptionHandler(1, lr_vectored_exception_handler);
    lr_trace("veh.install", "handle=%p", g_lr_vectored_exception_handler_handle);
}

static void lr_remove_vectored_exception_handler() {
    if (!g_lr_vectored_exception_handler_handle)
        return;

    RemoveVectoredExceptionHandler(g_lr_vectored_exception_handler_handle);
    lr_trace("veh.remove", "handle=%p", g_lr_vectored_exception_handler_handle);
    g_lr_vectored_exception_handler_handle = nullptr;
}
#endif

static void libretro_init_paths() {
    const char *sys = nullptr;
    const char *save = nullptr;
    const char *content = nullptr;

    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) && sys)
        libretro.system_dir = std::string(sys) + "/vita3k";
    else
        libretro.system_dir = ".";

    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save)
        libretro.save_dir = std::string(save) + "/vita3k";
    else
        libretro.save_dir = ".";

    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content) && content)
        libretro.content_dir = std::string(content);
    else
        libretro.content_dir = ".";
}

static struct {
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdClearColorImage CmdClearColorImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} vkfn = {};

static void libretro_vk_load_functions(const struct retro_hw_render_interface_vulkan *vulkan) {
    auto dpa = vulkan->get_device_proc_addr;
    auto ipa = vulkan->get_instance_proc_addr;
    VkDevice dev = vulkan->device;
    VkInstance inst = vulkan->instance;
#define LOAD_VK_DEV(fn) vkfn.fn = reinterpret_cast<PFN_vk##fn>(dpa(dev, "vk" #fn))
#define LOAD_VK_INST(fn) vkfn.fn = reinterpret_cast<PFN_vk##fn>(ipa(inst, "vk" #fn))
    LOAD_VK_INST(GetPhysicalDeviceMemoryProperties);
    LOAD_VK_DEV(CreateImage);
    LOAD_VK_DEV(DestroyImage);
    LOAD_VK_DEV(GetImageMemoryRequirements);
    LOAD_VK_DEV(AllocateMemory);
    LOAD_VK_DEV(FreeMemory);
    LOAD_VK_DEV(BindImageMemory);
    LOAD_VK_DEV(CreateImageView);
    LOAD_VK_DEV(DestroyImageView);
    LOAD_VK_DEV(CreateCommandPool);
    LOAD_VK_DEV(DestroyCommandPool);
    LOAD_VK_DEV(AllocateCommandBuffers);
    LOAD_VK_DEV(FreeCommandBuffers);
    LOAD_VK_DEV(BeginCommandBuffer);
    LOAD_VK_DEV(EndCommandBuffer);
    LOAD_VK_DEV(ResetCommandBuffer);
    LOAD_VK_DEV(CmdPipelineBarrier);
    LOAD_VK_DEV(CmdClearColorImage);
    LOAD_VK_DEV(CmdBlitImage);
    LOAD_VK_DEV(DeviceWaitIdle);
#undef LOAD_VK_DEV
#undef LOAD_VK_INST
}

static bool libretro_vk_validate_functions() {
    bool ok = true;
    auto check_fn = [&](const void *fn, const char *name) {
        if (!fn) {
            ok = false;
            lr_log(RETRO_LOG_ERROR, "Missing required Vulkan function pointer: %s\n", name);
            lr_trace("vk.fn_missing", "name=%s", name);
        }
    };

    check_fn(reinterpret_cast<const void *>(vkfn.GetPhysicalDeviceMemoryProperties), "vkGetPhysicalDeviceMemoryProperties");
    check_fn(reinterpret_cast<const void *>(vkfn.CreateImage), "vkCreateImage");
    check_fn(reinterpret_cast<const void *>(vkfn.DestroyImage), "vkDestroyImage");
    check_fn(reinterpret_cast<const void *>(vkfn.GetImageMemoryRequirements), "vkGetImageMemoryRequirements");
    check_fn(reinterpret_cast<const void *>(vkfn.AllocateMemory), "vkAllocateMemory");
    check_fn(reinterpret_cast<const void *>(vkfn.FreeMemory), "vkFreeMemory");
    check_fn(reinterpret_cast<const void *>(vkfn.BindImageMemory), "vkBindImageMemory");
    check_fn(reinterpret_cast<const void *>(vkfn.CreateImageView), "vkCreateImageView");
    check_fn(reinterpret_cast<const void *>(vkfn.DestroyImageView), "vkDestroyImageView");
    check_fn(reinterpret_cast<const void *>(vkfn.CreateCommandPool), "vkCreateCommandPool");
    check_fn(reinterpret_cast<const void *>(vkfn.DestroyCommandPool), "vkDestroyCommandPool");
    check_fn(reinterpret_cast<const void *>(vkfn.AllocateCommandBuffers), "vkAllocateCommandBuffers");
    check_fn(reinterpret_cast<const void *>(vkfn.FreeCommandBuffers), "vkFreeCommandBuffers");
    check_fn(reinterpret_cast<const void *>(vkfn.BeginCommandBuffer), "vkBeginCommandBuffer");
    check_fn(reinterpret_cast<const void *>(vkfn.EndCommandBuffer), "vkEndCommandBuffer");
    check_fn(reinterpret_cast<const void *>(vkfn.ResetCommandBuffer), "vkResetCommandBuffer");
    check_fn(reinterpret_cast<const void *>(vkfn.CmdPipelineBarrier), "vkCmdPipelineBarrier");
    check_fn(reinterpret_cast<const void *>(vkfn.CmdClearColorImage), "vkCmdClearColorImage");
    check_fn(reinterpret_cast<const void *>(vkfn.CmdBlitImage), "vkCmdBlitImage");
    check_fn(reinterpret_cast<const void *>(vkfn.DeviceWaitIdle), "vkDeviceWaitIdle");
    return ok;
}

static uint32_t libretro_vk_find_memory_type(VkPhysicalDevice gpu, uint32_t type_bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkfn.GetPhysicalDeviceMemoryProperties(gpu, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return 0;
}

static void libretro_vk_create_presentation_resources() {
    auto &vkp = libretro.vk_presentation;
    const auto *vulkan = vkp.vulkan;
    if (!vulkan)
        return;

    VkDevice device = vulkan->device;
    VkPhysicalDevice gpu = vulkan->gpu;
    const uint32_t w = vkp.width;
    const uint32_t h = vkp.height;

    uint32_t mask = vulkan->get_sync_index_mask(vulkan->handle);
    vkp.num_images = 0;
    for (uint32_t i = 0; i < 32; i++)
        if (mask & (1u << i))
            vkp.num_images = i + 1;
    if (vkp.num_images > LIBRETRO_MAX_SWAPCHAIN)
        vkp.num_images = LIBRETRO_MAX_SWAPCHAIN;
    if (vkp.num_images == 0)
        vkp.num_images = 3;

    lr_log(RETRO_LOG_INFO, "Creating %u presentation images (%ux%u)\n", vkp.num_images, w, h);
    for (uint32_t i = 0; i < vkp.num_images; i++) {
        VkImageCreateInfo img_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        img_info.imageType = VK_IMAGE_TYPE_2D;
        img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        img_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        img_info.extent = { w, h, 1 };
        img_info.mipLevels = 1;
        img_info.arrayLayers = 1;
        img_info.samples = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        vkfn.CreateImage(device, &img_info, nullptr, &vkp.vk_images[i]);

        VkMemoryRequirements mem_reqs;
        vkfn.GetImageMemoryRequirements(device, vkp.vk_images[i], &mem_reqs);

        VkMemoryAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex = libretro_vk_find_memory_type(gpu, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkfn.AllocateMemory(device, &alloc_info, nullptr, &vkp.vk_memory[i]);
        vkfn.BindImageMemory(device, vkp.vk_images[i], vkp.vk_memory[i], 0);

        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image = vkp.vk_images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        view_info.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };

        vkfn.CreateImageView(device, &view_info, nullptr, &vkp.vk_views[i]);

        vkp.images[i].image_view = vkp.vk_views[i];
        vkp.images[i].image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkp.images[i].create_info = view_info;
    }

    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.queueFamilyIndex = vulkan->queue_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkfn.CreateCommandPool(device, &pool_info, nullptr, &vkp.cmd_pool);

    VkCommandBufferAllocateInfo cmd_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmd_alloc.commandPool = vkp.cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = vkp.num_images;
    vkfn.AllocateCommandBuffers(device, &cmd_alloc, vkp.cmd_buffers);

}

static void libretro_vk_destroy_presentation_resources() {
    auto &vkp = libretro.vk_presentation;
    const auto *vulkan = vkp.vulkan;
    if (!vulkan || !vulkan->device)
        return;

    VkDevice device = vulkan->device;
    vkfn.DeviceWaitIdle(device);

    if (vkp.cmd_pool) {
        vkfn.FreeCommandBuffers(device, vkp.cmd_pool, vkp.num_images, vkp.cmd_buffers);
        vkfn.DestroyCommandPool(device, vkp.cmd_pool, nullptr);
        vkp.cmd_pool = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < vkp.num_images; i++) {
        if (vkp.vk_views[i]) vkfn.DestroyImageView(device, vkp.vk_views[i], nullptr);
        if (vkp.vk_images[i]) vkfn.DestroyImage(device, vkp.vk_images[i], nullptr);
        if (vkp.vk_memory[i]) vkfn.FreeMemory(device, vkp.vk_memory[i], nullptr);
        vkp.vk_views[i] = VK_NULL_HANDLE;
        vkp.vk_images[i] = VK_NULL_HANDLE;
        vkp.vk_memory[i] = VK_NULL_HANDLE;
    }
    vkp.num_images = 0;

}

static void context_reset_vulkan() {
    lr_trace("context_reset_vulkan.enter", "renderer_ready=%d emuenv=%d", libretro.renderer_ready ? 1 : 0, libretro.emuenv ? 1 : 0);

    const struct retro_hw_render_interface_vulkan *vulkan = nullptr;
    if (!libretro.environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&vulkan) || !vulkan) {
        lr_log(RETRO_LOG_ERROR, "Failed to get Vulkan HW render interface!\n");
        lr_trace("context_reset_vulkan.error", "RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE failed");
        return;
    }

    if (vulkan->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN) {
        lr_log(RETRO_LOG_ERROR, "HW render interface is not Vulkan (got %u)!\n", vulkan->interface_type);
        lr_trace("context_reset_vulkan.error", "interface_type=%u", vulkan->interface_type);
        return;
    }

    lr_trace("context_reset_vulkan.interface",
        "instance=%p device=%p gpu=%p queue=%p queue_index=%u handle=%p",
        vulkan->instance,
        vulkan->device,
        vulkan->gpu,
        vulkan->queue,
        vulkan->queue_index,
        vulkan->handle);

    libretro.vk_presentation.vulkan = vulkan;
    libretro_vk_load_functions(vulkan);
    if (!libretro_vk_validate_functions()) {
        lr_trace("context_reset_vulkan.error", "required Vulkan functions missing");
        return;
    }

    LibretroVulkanHandles handles;
    handles.instance = vulkan->instance;
    handles.gpu = vulkan->gpu;
    handles.device = vulkan->device;
    handles.queue = vulkan->queue;
    handles.queue_family_index = vulkan->queue_index;
    handles.get_instance_proc_addr = vulkan->get_instance_proc_addr;
    handles.get_device_proc_addr = vulkan->get_device_proc_addr;
    set_libretro_vulkan_handles(handles);

    if (libretro.emuenv) {
        EmuEnvState &emuenv = *libretro.emuenv;
        if (!emuenv.renderer) {
            if (!renderer::init(emuenv.renderer, renderer::Backend::Vulkan, emuenv.cfg, *libretro.root_paths)) {
                lr_log(RETRO_LOG_ERROR, "Failed to create Vulkan renderer!\n");
                return;
            }
            set_libretro_queue_lock(emuenv.renderer.get(), vulkan->handle,
                vulkan->lock_queue, vulkan->unlock_queue);
            emuenv.renderer->late_init(emuenv.cfg, emuenv.io.app_path, emuenv.mem);
            if (!emuenv.io.title_id.empty())
                emuenv.renderer->set_app(emuenv.io.title_id.c_str(), emuenv.self_name.c_str());
        }
    }

    libretro_vk_create_presentation_resources();
    libretro.renderer_ready = true;
    lr_log(RETRO_LOG_INFO, "Vulkan context reset complete\n");
}

static void context_reset_opengl() {
    lr_trace("context_reset_opengl.enter", "starting");
    
    auto get_proc = libretro.hw_render.get_proc_address;
    if (!get_proc) {
        lr_log(RETRO_LOG_ERROR, "No get_proc_address callback from frontend!\n");
        lr_trace("context_reset_opengl.error", "no get_proc_address");
        return;
    }
    lr_trace("context_reset_opengl.have_get_proc", "get_proc=%p", (void*)get_proc);

    lr_trace("context_reset_opengl.loading_glad", "calling gladLoadGLLoader");
    if (!gladLoadGLLoader((GLADloadproc)get_proc)) {
        lr_log(RETRO_LOG_ERROR, "Failed to load OpenGL functions via glad!\n");
        lr_trace("context_reset_opengl.error", "gladLoadGLLoader failed");
        return;
    }
    lr_trace("context_reset_opengl.glad_loaded", "OpenGL functions loaded successfully");

    // Log OpenGL version and renderer info for Mesa debugging
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    lr_log(RETRO_LOG_INFO, "OpenGL - Version: %s, Vendor: %s, Renderer: %s\n", 
        version ? version : "NULL", 
        vendor ? vendor : "NULL", 
        renderer ? renderer : "NULL");
    lr_trace("context_reset_opengl.gl_info", "version=%s vendor=%s renderer=%s",
        version ? version : "NULL",
        vendor ? vendor : "NULL", 
        renderer ? renderer : "NULL");

    // Check for Mesa specifically
    bool is_mesa = (vendor && strstr(vendor, "Mesa") != nullptr) ||
                   (renderer && strstr(renderer, "Mesa") != nullptr);
    lr_trace("context_reset_opengl.mesa_check", "is_mesa=%d", is_mesa ? 1 : 0);

    if (libretro.emuenv) {
        EmuEnvState &emuenv = *libretro.emuenv;
        if (!emuenv.renderer) {
            lr_trace("context_reset_opengl.creating_renderer", "backend=OpenGL");
            if (!renderer::init(emuenv.renderer, renderer::Backend::OpenGL, emuenv.cfg, *libretro.root_paths)) {
                lr_log(RETRO_LOG_ERROR, "Failed to create OpenGL renderer!\n");
                lr_trace("context_reset_opengl.error", "renderer::init failed");
                return;
            }
            lr_trace("context_reset_opengl.renderer_created", "success");
            
            lr_trace("context_reset_opengl.late_init", "calling renderer late_init");
            emuenv.renderer->late_init(emuenv.cfg, emuenv.io.app_path, emuenv.mem);
            lr_trace("context_reset_opengl.late_init_complete", "success");
            
            if (!emuenv.io.title_id.empty()) {
                lr_trace("context_reset_opengl.set_app", "title_id=%s", emuenv.io.title_id.c_str());
                emuenv.renderer->set_app(emuenv.io.title_id.c_str(), emuenv.self_name.c_str());
            }
        } else {
            lr_trace("context_reset_opengl.renderer_exists", "skipping creation");
        }
    } else {
        lr_trace("context_reset_opengl.error", "emuenv is null");
        return;
    }

    lr_trace("context_reset_opengl.creating_screen_texture", "size=960x544");
    if (libretro.gl_screen_texture == 0) {
        glGenTextures(1, &libretro.gl_screen_texture);
        glBindTexture(GL_TEXTURE_2D, libretro.gl_screen_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        lr_trace("context_reset_opengl.screen_texture_created", "texture_id=%u", libretro.gl_screen_texture);
    } else {
        lr_trace("context_reset_opengl.screen_texture_exists", "texture_id=%u", libretro.gl_screen_texture);
    }

    libretro.renderer_ready = true;
    lr_log(RETRO_LOG_INFO, "OpenGL context reset complete\n");
    lr_trace("context_reset_opengl.complete", "success renderer_ready=1");
}

static void context_reset_finalize() {
    lr_trace("context_reset_finalize.enter",
        "app_started=%d pending_main_module_id=%d active_context=%d renderer_ready=%d",
        libretro.app_started ? 1 : 0,
        libretro.pending_main_module_id,
        static_cast<int>(libretro.active_context),
        libretro.renderer_ready ? 1 : 0);

    lr_trace("context_reset_finalize.check_conditions",
        "emuenv=%d app_started=%d pending_main_module_id=%d",
        libretro.emuenv ? 1 : 0,
        libretro.app_started ? 1 : 0,
        libretro.pending_main_module_id);

    if (libretro.emuenv && !libretro.app_started && libretro.pending_main_module_id >= 0) {
        EmuEnvState &emuenv = *libretro.emuenv;
        lr_log(RETRO_LOG_INFO,
            "context_reset_finalize: starting app (pending_main_module_id=%d)\n",
            libretro.pending_main_module_id);
        
        lr_trace("context_reset_finalize.before_libretro_run_app", "calling libretro_run_app");
        ExitCode run_err = libretro_run_app(emuenv, libretro.pending_main_module_id);
        lr_trace("context_reset_finalize.after_libretro_run_app", "run_err=%d", static_cast<int>(run_err));
        
        if (run_err != Success) {
            lr_log(RETRO_LOG_ERROR, "Failed to start game (error %d)!\n", (int)run_err);
            lr_trace("context_reset_finalize.app_start_failed", "error=%d", static_cast<int>(run_err));
        } else {
            libretro.app_started = true;
            libretro.pending_main_module_id = -1;
            lr_log(RETRO_LOG_INFO, "context_reset_finalize: app start complete\n");
            lr_trace("context_reset_finalize.app_start_success");
        }
    } else {
        lr_trace("context_reset_finalize.skip_app_start", "conditions not met");
    }

    lr_trace("context_reset_finalize.before_render_thread", "active_context=%d renderer=%p render_thread=%p",
        static_cast<int>(libretro.active_context),
        libretro.emuenv && libretro.emuenv->renderer ? static_cast<void*>(libretro.emuenv->renderer.get()) : nullptr,
        static_cast<void*>(libretro.render_thread.get()));

    if (libretro.active_context == RETRO_HW_CONTEXT_VULKAN
        && libretro.emuenv && libretro.emuenv->renderer && !libretro.render_thread) {
        lr_trace("context_reset_finalize.starting_render_thread");
        libretro.render_thread_running = true;
        libretro.render_thread = std::make_unique<std::thread>([]() {
            EmuEnvState &emuenv = *libretro.emuenv;
            while (libretro.render_thread_running.load()) {
                if (!emuenv.renderer)
                    break;
                emuenv.renderer->should_display = false;
                renderer::process_batches(*emuenv.renderer, emuenv.renderer->features, emuenv.mem, emuenv.cfg);

                if (emuenv.renderer->should_display) {
                    DisplayFrameInfo frame;
                    {
                        std::lock_guard<std::mutex> guard(emuenv.display.display_info_mutex);
                        frame = emuenv.display.next_rendered_frame;
                    }

                    auto *vk_state = dynamic_cast<renderer::vulkan::VKState *>(emuenv.renderer.get());
                    if (vk_state && frame.base) {
                        renderer::vulkan::Viewport viewport;
                        viewport.width = static_cast<uint32_t>(frame.image_size.x * vk_state->res_multiplier);
                        viewport.height = static_cast<uint32_t>(frame.image_size.y * vk_state->res_multiplier);

                        vk::Image surface_image;
                        vk::ImageView surface_view = vk_state->surface_cache.sourcing_color_surface_for_presentation(
                            frame.base, frame.pitch, viewport, &surface_image);

                        if (surface_view && surface_image) {
                            std::lock_guard<std::mutex> lock(libretro.rendered_frame_mutex);
                            libretro.vk_surface_image = static_cast<VkImage>(surface_image);
                            libretro.vk_surface_width = viewport.width;
                            libretro.vk_surface_height = viewport.height;
                            libretro.vk_surface_offset_x = viewport.offset_x;
                            libretro.vk_surface_offset_y = viewport.offset_y;
                            libretro.has_new_frame = true;
                        }
                    }
                }
            }
        });
    }
    
    lr_trace("context_reset_finalize.complete", "finished");
}

static void context_reset() {
    lr_log(RETRO_LOG_INFO, "context_reset: active_context=%d\n", (int)libretro.active_context);

    if (libretro.active_context == RETRO_HW_CONTEXT_VULKAN) {
        context_reset_vulkan();
    } else if (libretro.active_context == RETRO_HW_CONTEXT_OPENGL_CORE
            || libretro.active_context == RETRO_HW_CONTEXT_OPENGL) {
        context_reset_opengl();
    } else {
        lr_log(RETRO_LOG_ERROR, "context_reset: unknown active_context %d\n", (int)libretro.active_context);
        return;
    }

    if (libretro.renderer_ready)
        context_reset_finalize();
}

static void context_destroy() {
    libretro.renderer_ready = false;

    stop_render_thread();

    if (libretro.active_context == RETRO_HW_CONTEXT_VULKAN) {
        libretro_vk_destroy_presentation_resources();
        libretro.vk_presentation.vulkan = nullptr;
    } else if (libretro.active_context == RETRO_HW_CONTEXT_OPENGL_CORE
            || libretro.active_context == RETRO_HW_CONTEXT_OPENGL) {
        if (libretro.gl_screen_texture) {
            glDeleteTextures(1, &libretro.gl_screen_texture);
            libretro.gl_screen_texture = 0;
        }
    }

    lr_log(RETRO_LOG_INFO, "context_destroy\n");
}

static void stop_render_thread() {
    if (!libretro.render_thread)
        return;

    lr_log(RETRO_LOG_INFO,
        "Stopping libretro render thread: running=%d, joinable=%d\n",
        libretro.render_thread_running.load() ? 1 : 0,
        libretro.render_thread->joinable() ? 1 : 0);

    libretro.render_thread_running = false;
    if (libretro.render_thread->joinable())
        libretro.render_thread->join();

    libretro.render_thread.reset();
    lr_log(RETRO_LOG_INFO, "Libretro render thread stopped\n");
}

static void request_guest_shutdown(EmuEnvState &emuenv, const char *reason) {
    const char *shutdown_reason = reason ? reason : "unknown";

    std::size_t guest_thread_count_before = 0;
    {
        std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        guest_thread_count_before = emuenv.kernel.threads.size();
    }

    lr_log(RETRO_LOG_INFO,
        "Guest shutdown begin (%s): app_started=%d renderer=%d display_abort=%d guest_threads=%zu\n",
        shutdown_reason,
        libretro.app_started ? 1 : 0,
        emuenv.renderer ? 1 : 0,
        emuenv.display.abort.load() ? 1 : 0,
        guest_thread_count_before);

    if (emuenv.renderer) {
        lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): renderer preclose_action begin\n", shutdown_reason);
        emuenv.renderer->preclose_action();
        lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): renderer preclose_action complete\n", shutdown_reason);
    } else {
        lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): renderer already null before preclose\n", shutdown_reason);
    }

    emuenv.kernel.exit_delete_all_threads();
    lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): exit_delete_all_threads issued\n", shutdown_reason);

    if (!emuenv.kernel.wait_for_all_threads_exit(5000)) {
        lr_log(RETRO_LOG_WARN,
            "Guest shutdown (%s): timed out waiting for guest thread drain\n",
            shutdown_reason);
        
        // Force-terminate remaining threads
        std::size_t forced_count = 0;
        {
            std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            for (auto &[id, thread] : emuenv.kernel.threads) {
                lr_log(RETRO_LOG_WARN, "Guest shutdown (%s): force-terminating stuck thread '{}' (id={})\n",
                    shutdown_reason, thread->name, id);
                thread->exit_delete(false);
                forced_count++;
            }
        }
        lr_log(RETRO_LOG_WARN, "Guest shutdown (%s): force-terminated %zu stuck threads\n",
            shutdown_reason, forced_count);
        
        // Give them a final chance to exit
        emuenv.kernel.wait_for_all_threads_exit(1000);
    } else {
        lr_log(RETRO_LOG_INFO,
            "Guest shutdown (%s): guest threads drained\n",
            shutdown_reason);
    }

    std::size_t guest_thread_count_after = 0;
    {
        std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        guest_thread_count_after = emuenv.kernel.threads.size();
    }
    lr_log(RETRO_LOG_INFO,
        "Guest shutdown (%s): guest thread count after wait=%zu\n",
        shutdown_reason,
        guest_thread_count_after);

    emuenv.gxm.display_queue.abort();
    lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): gxm display queue aborted\n", shutdown_reason);

    emuenv.display.abort = true;
    lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): display.abort set to true\n", shutdown_reason);

    if (emuenv.renderer) {
        emuenv.renderer->should_display = true;
        emuenv.renderer->notification_ready.notify_all();
        lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): renderer notifications broadcast\n", shutdown_reason);
    } else {
        lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): renderer already null\n", shutdown_reason);
    }

    {
        std::lock_guard<std::mutex> lock(libretro.rendered_frame_mutex);
        libretro.vk_surface_image = VK_NULL_HANDLE;
        libretro.vk_surface_width = 0;
        libretro.vk_surface_height = 0;
        libretro.vk_surface_offset_x = 0;
        libretro.vk_surface_offset_y = 0;
        libretro.has_new_frame = false;
    }
    lr_log(RETRO_LOG_INFO, "Guest shutdown (%s): cached presented frame cleared\n", shutdown_reason);
}

static bool perform_core_reset() {
    lr_log(RETRO_LOG_INFO,
        "perform_core_reset: begin (game_loaded=%d, emuenv=%d, cfg=%d, root=%d, context=%d)\n",
        libretro.game_loaded ? 1 : 0,
        libretro.emuenv ? 1 : 0,
        libretro.cfg ? 1 : 0,
        libretro.root_paths ? 1 : 0,
        static_cast<int>(libretro.active_context));

    if (!libretro.game_loaded || !libretro.emuenv || !libretro.cfg || !libretro.root_paths) {
        lr_log(RETRO_LOG_WARN, "perform_core_reset: prerequisites missing, skipping reset\n");
        return false;
    }

    stop_render_thread();
    request_guest_shutdown(*libretro.emuenv, "retro_reset");
    context_destroy();

    libretro.emuenv.reset();
    libretro.renderer_ready = false;
    libretro.app_started = false;
    libretro.pending_main_module_id = -1;

    libretro.emuenv = std::make_unique<EmuEnvState>();
    EmuEnvState &emuenv = *libretro.emuenv;

    if (!libretro_init_emuenv(emuenv, *libretro.root_paths)) {
        lr_log(RETRO_LOG_ERROR, "perform_core_reset: failed to reinitialize EmuEnvState\n");
        libretro.game_loaded = false;
        libretro.emuenv.reset();
        return false;
    }

    apply_config_changes();

    int32_t main_module_id = -1;
    const ExitCode load_err = libretro_load_app(main_module_id, emuenv);
    if (load_err != Success) {
        lr_log(RETRO_LOG_ERROR, "perform_core_reset: failed to reload app (error %d)\n", static_cast<int>(load_err));
        libretro.game_loaded = false;
        libretro.emuenv.reset();
        return false;
    }

    libretro.pending_main_module_id = main_module_id;
    libretro.app_started = false;

    context_reset();

    lr_log(RETRO_LOG_INFO,
        "perform_core_reset: complete (renderer_ready=%d, app_started=%d, pending_main_module_id=%d)\n",
        libretro.renderer_ready ? 1 : 0,
        libretro.app_started ? 1 : 0,
        libretro.pending_main_module_id);
    return true;
}

static const VkApplicationInfo *libretro_vk_get_application_info() {
    static const VkApplicationInfo info = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        "Vita3K",
        0,
        "Vita3K",
        0,
        VK_MAKE_API_VERSION(0, 1, 1, 0),
    };
    return &info;
}

static bool try_set_hw_vulkan() {
    memset(&libretro.hw_render, 0, sizeof(libretro.hw_render));
    libretro.hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
    libretro.hw_render.version_major = VK_MAKE_API_VERSION(0, 1, 1, 0);
    libretro.hw_render.version_minor = 0;
    libretro.hw_render.context_reset = context_reset;
    libretro.hw_render.context_destroy = context_destroy;
    libretro.hw_render.cache_context = true;
    libretro.hw_render.bottom_left_origin = false;

    if (!libretro.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &libretro.hw_render)) {
        lr_log(RETRO_LOG_WARN, "Frontend rejected Vulkan HW context\n");
        return false;
    }

    static const struct retro_hw_render_context_negotiation_interface_vulkan negotiation = {
        RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
        RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
        libretro_vk_get_application_info,
        nullptr,
        nullptr,
    };
    libretro.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void *)&negotiation);

    libretro.active_context = RETRO_HW_CONTEXT_VULKAN;
    return true;
}

static bool try_set_hw_opengl() {
    lr_trace("try_set_hw_opengl.enter", "requesting OpenGL Core 4.4");
    
    memset(&libretro.hw_render, 0, sizeof(libretro.hw_render));
    libretro.hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    libretro.hw_render.version_major = 4;
    libretro.hw_render.version_minor = 4;
    libretro.hw_render.context_reset = context_reset;
    libretro.hw_render.context_destroy = context_destroy;
    libretro.hw_render.cache_context = true;
    libretro.hw_render.bottom_left_origin = true;
    libretro.hw_render.depth = false;
    libretro.hw_render.stencil = false;

    lr_trace("try_set_hw_opengl.setting_hw_render", "calling RETRO_ENVIRONMENT_SET_HW_RENDER");
    if (!libretro.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &libretro.hw_render)) {
        lr_log(RETRO_LOG_WARN, "Frontend rejected OpenGL Core 4.4 HW context\n");
        lr_trace("try_set_hw_opengl.rejected", "OpenGL Core 4.4 rejected by frontend");
        return false;
    }

    libretro.active_context = RETRO_HW_CONTEXT_OPENGL_CORE;
    lr_trace("try_set_hw_opengl.success", "OpenGL Core 4.4 context accepted");
    return true;
}

static bool libretro_init_hw_render() {
    const char *backend_option = get_core_option_value("vita3k_backend_renderer", "Vulkan");
    const bool force_opengl = (strcmp(backend_option, "OpenGL") == 0);
    const bool force_vulkan = (strcmp(backend_option, "Vulkan") == 0);

    if (force_opengl) {
        if (try_set_hw_opengl()) return true;
        if (try_set_hw_vulkan()) return true;
    } else if (force_vulkan) {
        if (try_set_hw_vulkan()) return true;
        if (try_set_hw_opengl()) return true;
    }

    unsigned preferred = RETRO_HW_CONTEXT_NONE;
    if (!libretro.environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred))
        preferred = RETRO_HW_CONTEXT_VULKAN;

    bool is_gl_preferred = (preferred == RETRO_HW_CONTEXT_OPENGL_CORE
                         || preferred == RETRO_HW_CONTEXT_OPENGL
                         || preferred == RETRO_HW_CONTEXT_OPENGLES2
                         || preferred == RETRO_HW_CONTEXT_OPENGLES3
                         || preferred == RETRO_HW_CONTEXT_OPENGLES_VERSION);

    if (is_gl_preferred) {
        if (try_set_hw_opengl()) return true;
        if (try_set_hw_vulkan()) return true;
    } else {
        if (try_set_hw_vulkan()) return true;
        if (try_set_hw_opengl()) return true;
    }

    lr_log(RETRO_LOG_ERROR, "Failed to set any HW render context! Neither Vulkan nor OpenGL accepted.\n");
    return false;
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    libretro.environ_cb = cb;

    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    libretro_set_core_options();
    libretro_log_init();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    libretro.video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    libretro.audio_sample_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    libretro.audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    libretro.input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    libretro.input_state_cb = cb;
}

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "Vita3K";
    info->library_version = app_version;
    info->valid_extensions = NULL;
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = 960;
    info->geometry.base_height = 544;
    info->geometry.max_width = 960 * 4;
    info->geometry.max_height = 544 * 4;
    info->geometry.aspect_ratio = 960.0f / 544.0f;

    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_init(void) {
    libretro_log_init();
    libretro_logging_init();

    libretro_init_paths();
#ifdef _WIN32
    lr_install_vectored_exception_handler();
#endif
    libretro_init_input_descriptors();
    libretro_setup_controller_info();

    lr_trace("retro_init", "system_dir=%s save_dir=%s content_dir=%s", libretro.system_dir.c_str(), libretro.save_dir.c_str(), libretro.content_dir.c_str());

    libretro.initialized = true;
}

RETRO_API void retro_deinit(void) {
    lr_log(RETRO_LOG_INFO, "retro_deinit()\n");
    lr_trace("retro_deinit.enter",
        "initialized=%d game_loaded=%d emuenv=%d app_started=%d",
        libretro.initialized ? 1 : 0,
        libretro.game_loaded ? 1 : 0,
        libretro.emuenv ? 1 : 0,
        libretro.app_started ? 1 : 0);

    if (libretro.emuenv) {
        stop_render_thread();
        request_guest_shutdown(*libretro.emuenv, "retro_deinit");
    }

    libretro.emuenv.reset();
    libretro.cfg.reset();
    libretro.root_paths.reset();
    libretro.initialized = false;
    libretro.game_loaded = false;
    libretro.app_started = false;
    libretro.pending_main_module_id = -1;
    libretro.reset_requested = false;

#ifdef _WIN32
    lr_remove_vectored_exception_handler();
#endif
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->path) {
        lr_log(RETRO_LOG_ERROR, "No game path provided\n");
        return false;
    }

    lr_log(RETRO_LOG_INFO, "retro_load_game: %s\n", game->path);
    lr_trace("retro_load_game.begin", "path=%s", game->path);
    libretro.game_path = game->path;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!libretro.environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        lr_log(RETRO_LOG_ERROR, "XRGB8888 pixel format not supported\n");
        return false;
    }

    if (!libretro_init_hw_render()) {
        lr_log(RETRO_LOG_ERROR, "retro_load_game: failed to initialize HW render context\n");
        lr_trace("retro_load_game.error", "libretro_init_hw_render failed");
        return false;
    }
    lr_trace("retro_load_game.hw_context", "active_context=%d", static_cast<int>(libretro.active_context));

    const fs::path pref_path = fs::path(libretro.system_dir);
    const fs::path system_dir = fs::path(libretro.system_dir);
    const fs::path game_path = fs_utils::utf8_to_path(libretro.game_path);
    fs::create_directories(pref_path);

    if (!ensure_firmware_installed(pref_path, system_dir)) {
        lr_log(RETRO_LOG_WARN, "Firmware not found — expect errors\n");
        lr_trace("retro_load_game.firmware", "firmware not found at %s", system_dir.generic_string().c_str());
    }

    const int sys_lang = 1;
    GameInstallResult install_result = ensure_game_installed(game_path, pref_path, sys_lang);

    if (!install_result.success) {
        lr_log(RETRO_LOG_ERROR, "Game installation/verification failed.\n");
        lr_trace("retro_load_game.error", "ensure_game_installed failed for %s", game_path.generic_string().c_str());
        return false;
    }

    lr_trace("retro_load_game.install",
        "title_id=%s title=%s",
        install_result.title_id.c_str(),
        install_result.title.c_str());

    libretro.installed_title_id = install_result.title_id;
    libretro.installed_title = install_result.title;

    libretro.root_paths = std::make_unique<Root>();
    Root &root = *libretro.root_paths;

    root.set_base_path(pref_path);
    root.set_pref_path(pref_path);
    root.set_log_path(pref_path / "log");
    root.set_config_path(pref_path / "config");
    root.set_cache_path(pref_path / "cache");
    root.set_shared_path(pref_path);
    root.set_patch_path(pref_path / "patch");

    const fs::path core_dir = fs::path(libretro.system_dir).parent_path();
    if (fs::exists(core_dir / "data"))
        root.set_static_assets_path(core_dir);
    else if (fs::exists(pref_path / "data"))
        root.set_static_assets_path(pref_path);

    app::init_paths(root);
    fs::create_directories(pref_path / "ux0" / "app");
    fs::create_directories(pref_path / "ux0" / "data");
    fs::create_directories(pref_path / "ux0" / "user" / "00" / "savedata");
    fs::create_directories(pref_path / "ux0" / "addcont");
    fs::create_directories(pref_path / "ux0" / "patch");

    libretro.cfg = std::make_unique<Config>();
    Config &cfg = *libretro.cfg;
    cfg.pref_path = root.get_pref_path().generic_string();
    cfg.backend_renderer = (libretro.active_context == RETRO_HW_CONTEXT_VULKAN) ? "Vulkan" : "OpenGL";
    cfg.audio_backend = "Libretro";
    cfg.console = false;
    cfg.show_gui = false;
    cfg.overwrite_config = false;
    cfg.run_app_path = libretro.installed_title_id;

    // Prime config from libretro core options before EmuEnv initialization so
    // restart-sensitive settings (CPU/modules/renderer/memory mapping) are
    // used during app::init/app::late_init and module loading.
    read_core_options();
    apply_config_changes();

    libretro.emuenv = std::make_unique<EmuEnvState>();
    EmuEnvState &emuenv = *libretro.emuenv;

    if (!libretro_init_emuenv(emuenv, root)) {
        lr_log(RETRO_LOG_ERROR, "Failed to initialize emulator environment!\n");
        lr_trace("retro_load_game.error", "libretro_init_emuenv failed");
        libretro.emuenv.reset();
        return false;
    }

    // Apply live-changeable settings on initialized subsystems.
    apply_config_changes();

    int32_t main_module_id = -1;
    ExitCode load_err = libretro_load_app(main_module_id, emuenv);
    if (load_err != Success) {
        lr_log(RETRO_LOG_ERROR, "Failed to load game (error %d)!\n", (int)load_err);
        lr_trace("retro_load_game.error", "libretro_load_app failed code=%d", static_cast<int>(load_err));
        libretro.emuenv.reset();
        return false;
    }

    libretro.pending_main_module_id = main_module_id;
    libretro.app_started = false;
    libretro.reset_requested = false;
    
    libretro.game_loaded = true;

    lr_trace("retro_load_game.complete",
        "game_loaded=%d app_started=%d pending_main_module_id=%d",
        libretro.game_loaded ? 1 : 0,
        libretro.app_started ? 1 : 0,
        libretro.pending_main_module_id);

    return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void) {
    lr_log(RETRO_LOG_INFO, "retro_unload_game()\n");
    lr_trace("retro_unload_game.enter", "game_loaded=%d emuenv=%d", libretro.game_loaded ? 1 : 0, libretro.emuenv ? 1 : 0);

    if (libretro.emuenv) {
        stop_render_thread();
        request_guest_shutdown(*libretro.emuenv, "retro_unload_game");
    }

    libretro.emuenv.reset();
    libretro.game_loaded = false;
    libretro.app_started = false;
    libretro.pending_main_module_id = -1;
    libretro.reset_requested = false;
    libretro.installed_title_id.clear();
    libretro.installed_title.clear();
}

RETRO_API void retro_run(void) {
    if (!libretro.game_loaded || !libretro.emuenv)
        return;

    static std::atomic<uint64_t> run_counter{ 0 };
    const uint64_t run_index = run_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool trace_this_tick = (run_index <= 240) || ((run_index % 300) == 0);
    if (trace_this_tick) {
        lr_trace("retro_run.tick",
            "tick=%llu app_started=%d renderer_ready=%d active_context=%d render_thread=%d",
            static_cast<unsigned long long>(run_index),
            libretro.app_started ? 1 : 0,
            libretro.renderer_ready ? 1 : 0,
            static_cast<int>(libretro.active_context),
            libretro.render_thread ? 1 : 0);
    }

    if (libretro.reset_requested.exchange(false)) {
        lr_log(RETRO_LOG_INFO, "retro_run: processing deferred reset request\n");
        if (!perform_core_reset()) {
            lr_log(RETRO_LOG_WARN, "retro_run: deferred reset failed\n");
            return;
        }
        if (!libretro.game_loaded || !libretro.emuenv)
            return;
    }

    // Check for core options updates
    bool updated = false;
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        read_core_options();
        apply_config_changes();
    }

    EmuEnvState &emuenv = *libretro.emuenv;

    if (libretro.input_poll_cb)
        libretro.input_poll_cb();
    libretro_poll_input();

    if (libretro.app_started)
        libretro_vblank_tick(emuenv);

    if (libretro.active_context != RETRO_HW_CONTEXT_VULKAN
        && libretro.renderer_ready && emuenv.renderer) {
        emuenv.renderer->should_display = false;
        renderer::process_batches(*emuenv.renderer, emuenv.renderer->features, emuenv.mem, emuenv.cfg);
    }

    auto &vkp = libretro.vk_presentation;
    const auto *vulkan = vkp.vulkan;

    if (libretro.renderer_ready && libretro.active_context == RETRO_HW_CONTEXT_VULKAN
        && vulkan && vkp.num_images > 0) {
        if (!libretro_vk_validate_functions()) {
            lr_trace("retro_run.vk_abort", "missing required Vulkan functions at tick=%llu", static_cast<unsigned long long>(run_index));
            return;
        }

        uint32_t new_mask = vulkan->get_sync_index_mask(vulkan->handle);
        uint32_t new_count = 0;
        for (uint32_t i = 0; i < 32; i++)
            if (new_mask & (1u << i))
                new_count = i + 1;
        if (new_count != vkp.num_images && new_count > 0) {
            lr_trace("retro_run.vk_swapchain_resize", "old_num_images=%u new_num_images=%u", vkp.num_images, new_count);
            libretro_vk_destroy_presentation_resources();
            libretro_vk_create_presentation_resources();
        }

        vulkan->wait_sync_index(vulkan->handle);
        uint32_t index = vulkan->get_sync_index(vulkan->handle);
        if (index >= vkp.num_images)
            index = 0;

        if (trace_this_tick) {
            lr_trace("retro_run.vk_sync",
                "tick=%llu sync_index=%u num_images=%u width=%u height=%u",
                static_cast<unsigned long long>(run_index),
                index,
                vkp.num_images,
                vkp.width,
                vkp.height);
        }

        VkCommandBuffer cmd = vkp.cmd_buffers[index];

        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkfn.ResetCommandBuffer(cmd, 0);
        vkfn.BeginCommandBuffer(cmd, &begin_info);

        VkImage src_image = VK_NULL_HANDLE;
        uint32_t src_w = 0, src_h = 0, src_ox = 0, src_oy = 0;
        {
            std::lock_guard<std::mutex> lock(libretro.rendered_frame_mutex);
            if (libretro.has_new_frame && libretro.vk_surface_image != VK_NULL_HANDLE) {
                src_image = libretro.vk_surface_image;
                src_w = libretro.vk_surface_width;
                src_h = libretro.vk_surface_height;
                src_ox = libretro.vk_surface_offset_x;
                src_oy = libretro.vk_surface_offset_y;
            }
        }

        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = vkp.vk_images[index];
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (src_image != VK_NULL_HANDLE && src_w > 0 && src_h > 0) {
            if (trace_this_tick) {
                lr_trace("retro_run.vk_blit",
                    "tick=%llu src_image=%p src_w=%u src_h=%u src_ox=%u src_oy=%u",
                    static_cast<unsigned long long>(run_index),
                    src_image,
                    src_w,
                    src_h,
                    src_ox,
                    src_oy);
            }

            VkImageMemoryBarrier src_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            src_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            src_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            src_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            src_barrier.image = src_image;
            src_barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &src_barrier);

            VkImageBlit blit_region = {};
            blit_region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit_region.srcOffsets[0] = { (int32_t)src_ox, (int32_t)src_oy, 0 };
            blit_region.srcOffsets[1] = { (int32_t)(src_ox + src_w), (int32_t)(src_oy + src_h), 1 };
            blit_region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit_region.dstOffsets[0] = { 0, 0, 0 };
            blit_region.dstOffsets[1] = { (int32_t)vkp.width, (int32_t)vkp.height, 1 };

            vkfn.CmdBlitImage(cmd, src_image, VK_IMAGE_LAYOUT_GENERAL,
                vkp.vk_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit_region, VK_FILTER_LINEAR);

        } else {
            VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkfn.CmdClearColorImage(cmd, vkp.vk_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        }

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkfn.EndCommandBuffer(cmd);

        vkp.images[index].image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vulkan->set_image(vulkan->handle, &vkp.images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
        vulkan->set_command_buffers(vulkan->handle, 1, &cmd);
        libretro.video_cb(RETRO_HW_FRAME_BUFFER_VALID, vkp.width, vkp.height, 0);
    } else if (libretro.renderer_ready
            && (libretro.active_context == RETRO_HW_CONTEXT_OPENGL_CORE
             || libretro.active_context == RETRO_HW_CONTEXT_OPENGL)) {
        GLuint fbo = (GLuint)libretro.hw_render.get_current_framebuffer();
        DisplayFrameInfo frame;
        {
            std::lock_guard<std::mutex> guard(emuenv.display.display_info_mutex);
            frame = emuenv.display.next_rendered_frame;
        }

        uint32_t width = frame.image_size.x > 0 ? static_cast<uint32_t>(frame.image_size.x) : 960;
        uint32_t height = frame.image_size.y > 0 ? static_cast<uint32_t>(frame.image_size.y) : 544;
        GLuint surface_tex = 0;
        float uvs[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

        auto *gl_state = dynamic_cast<renderer::gl::GLState *>(emuenv.renderer.get());
        if (gl_state && frame.base) {
            SceFVector2 tex_size;
            surface_tex = gl_state->surface_cache.sourcing_color_surface_for_presentation(
                frame.base, frame.image_size.x, frame.image_size.y, frame.pitch,
                uvs, gl_state->res_multiplier, tex_size);
            if (surface_tex) {
                width = static_cast<uint32_t>(frame.image_size.x * gl_state->res_multiplier);
                height = static_cast<uint32_t>(frame.image_size.y * gl_state->res_multiplier);

                const GLint standard_swizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
                glBindTexture(GL_TEXTURE_2D, surface_tex);
                glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, standard_swizzle);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);

        if (surface_tex) {
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            static GLuint read_fbo = 0;
            if (!read_fbo)
                glGenFramebuffers(1, &read_fbo);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface_tex, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);

            GLint src_y0 = static_cast<GLint>(uvs[1] * height / uvs[3]);
            GLint src_y1 = static_cast<GLint>(height);
            GLint src_x1 = static_cast<GLint>(width);

            glBlitFramebuffer(0, src_y0, src_x1, src_y1,
                              0, height, width, 0,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        libretro.video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
    } else {
        const int width = 960;
        const int height = 544;

        DisplayFrameInfo frame;
        {
            std::lock_guard<std::mutex> guard(emuenv.display.display_info_mutex);
            frame = emuenv.display.next_rendered_frame;
        }

        if (frame.base && frame.image_size.x > 0 && frame.image_size.y > 0) {
            const int fw = frame.image_size.x;
            const int fh = frame.image_size.y;
            const int pitch = frame.pitch > 0 ? frame.pitch : fw;
            const uint32_t *src = reinterpret_cast<const uint32_t *>(frame.base.get(emuenv.mem));

            if (src) {
                if (libretro.frame_buffer.size() != static_cast<size_t>(fw * fh))
                    libretro.frame_buffer.resize(fw * fh);

                // Convert ABGR8888 -> XRGB8888
                for (int y = 0; y < fh; y++) {
                    for (int x = 0; x < fw; x++) {
                        const uint32_t pixel = src[y * pitch + x];
                        const uint32_t r = (pixel >> 0) & 0xFF;
                        const uint32_t g = (pixel >> 8) & 0xFF;
                        const uint32_t b = (pixel >> 16) & 0xFF;
                        libretro.frame_buffer[y * fw + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
                    }
                }
                libretro.video_cb(libretro.frame_buffer.data(), fw, fh, fw * sizeof(uint32_t));
            } else {
                libretro.video_cb(nullptr, width, height, width * sizeof(uint32_t));
            }
        } else {
            if (libretro.frame_buffer.size() != static_cast<size_t>(width * height))
                libretro.frame_buffer.resize(width * height, 0);
            libretro.video_cb(libretro.frame_buffer.data(), width, height, width * sizeof(uint32_t));
        }
    }

    {
        static std::vector<int16_t> audio_buf;
        int num_frames = libretro_audio_drain(emuenv.audio, audio_buf);
        if (num_frames > 0 && libretro.audio_batch_cb) {
            libretro.audio_batch_cb(audio_buf.data(), num_frames);
        } else if (libretro.audio_batch_cb) {
            static int16_t silence[1600] = {};
            libretro.audio_batch_cb(silence, 800);
        }
    }
}

static void read_core_option(const char *key, const char *default_value, std::function<void(const char *)> apply_func) {
    if (!libretro.environ_cb || !apply_func)
        return;
    
    retro_variable variable = {};
    variable.key = key;
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value) {
        lr_log(RETRO_LOG_INFO, "Core option read: %s='%s'\n", key, variable.value);
        apply_func(variable.value);
    } else {
        // Use default value if variable not found
        lr_log(RETRO_LOG_INFO, "Core option read: %s missing, using default='%s'\n", key, default_value);
        apply_func(default_value);
    }
}

static void apply_config_changes() {
    if (!libretro.cfg)
        return;

    Config &cfg = *libretro.cfg;
    const bool have_emuenv = (libretro.emuenv != nullptr);
    EmuEnvState *emuenv = have_emuenv ? libretro.emuenv.get() : nullptr;

    const bool backend_changed = (cfg.backend_renderer != cfg.current_config.backend_renderer);
    const float resolution_delta = std::fabs(cfg.resolution_multiplier - cfg.current_config.resolution_multiplier);
    const bool resolution_changed = (resolution_delta > 0.001f);
    const bool memory_mapping_changed = (cfg.memory_mapping != cfg.current_config.memory_mapping);
    const bool high_accuracy_changed = (cfg.high_accuracy != cfg.current_config.high_accuracy);
    const bool modules_mode_changed = (cfg.modules_mode != cfg.current_config.modules_mode);
    const bool cpu_opt_changed = (cfg.cpu_opt != cfg.current_config.cpu_opt);

    const bool restart_required = backend_changed || resolution_changed || memory_mapping_changed
        || high_accuracy_changed || modules_mode_changed || cpu_opt_changed;

    if (restart_required && libretro.app_started) {
        lr_log(RETRO_LOG_INFO,
            "Restart check details: backend('%s'->'%s') res(%.3f->%.3f, delta=%.6f) mem_map('%s'->'%s') high_acc(%d->%d) modules(%d->%d) cpu_opt(%d->%d)\n",
            cfg.backend_renderer.c_str(),
            cfg.current_config.backend_renderer.c_str(),
            cfg.resolution_multiplier,
            cfg.current_config.resolution_multiplier,
            resolution_delta,
            cfg.memory_mapping.c_str(),
            cfg.current_config.memory_mapping.c_str(),
            cfg.high_accuracy ? 1 : 0,
            cfg.current_config.high_accuracy ? 1 : 0,
            cfg.modules_mode,
            cfg.current_config.modules_mode,
            cfg.cpu_opt ? 1 : 0,
            cfg.current_config.cpu_opt ? 1 : 0);
        lr_message("Vita3K: Some option changes require restarting content to fully apply.");
        lr_log(RETRO_LOG_WARN,
            "Core option change requires restart (backend=%d, res=%d, mem_map=%d, high_acc=%d, modules=%d, cpu_opt=%d)\n",
            backend_changed ? 1 : 0,
            resolution_changed ? 1 : 0,
            memory_mapping_changed ? 1 : 0,
            high_accuracy_changed ? 1 : 0,
            modules_mode_changed ? 1 : 0,
            cpu_opt_changed ? 1 : 0);
    }
    
    // Copy current_config values to main config
    cfg.cpu_opt = cfg.current_config.cpu_opt;
    cfg.modules_mode = cfg.current_config.modules_mode;
    cfg.lle_modules = cfg.current_config.lle_modules;
    cfg.audio_volume = cfg.current_config.audio_volume;
    cfg.ngs_enable = cfg.current_config.ngs_enable;
    cfg.pstv_mode = cfg.current_config.pstv_mode;
    cfg.sys_button = cfg.sys_button;
    cfg.fps_hack = cfg.current_config.fps_hack;
    cfg.file_loading_delay = cfg.current_config.file_loading_delay;
    cfg.show_touchpad_cursor = cfg.current_config.show_touchpad_cursor;
    cfg.psn_signed_in = cfg.current_config.psn_signed_in;
    
    // Renderer-specific config
    cfg.backend_renderer = cfg.current_config.backend_renderer;
    cfg.high_accuracy = cfg.current_config.high_accuracy;
    cfg.resolution_multiplier = cfg.current_config.resolution_multiplier;
    cfg.disable_surface_sync = cfg.current_config.disable_surface_sync;
    cfg.screen_filter = cfg.current_config.screen_filter;
    cfg.v_sync = cfg.current_config.v_sync;
    cfg.anisotropic_filtering = cfg.current_config.anisotropic_filtering;
    cfg.async_pipeline_compilation = cfg.current_config.async_pipeline_compilation;
    cfg.memory_mapping = cfg.current_config.memory_mapping;

    // Apply settings that can safely change while content is running.
    if (have_emuenv && emuenv->renderer) {
        emuenv->renderer->set_surface_sync_state(cfg.current_config.disable_surface_sync);
        emuenv->renderer->set_screen_filter(cfg.current_config.screen_filter);
        emuenv->renderer->set_anisotropic_filtering(cfg.current_config.anisotropic_filtering);
        emuenv->renderer->set_async_compilation(cfg.current_config.async_pipeline_compilation);

        // Resolution changes are restart-sensitive while ingame; apply immediately only before app start.
        if (!libretro.app_started)
            emuenv->renderer->res_multiplier = cfg.current_config.resolution_multiplier;
    }

    if (have_emuenv) {
        emuenv->display.fps_hack = cfg.current_config.fps_hack;
        emuenv->audio.set_global_volume(cfg.current_config.audio_volume / 100.0f);
    }
    
    // Log changes for debugging
    lr_log(RETRO_LOG_INFO, "Applied core options changes:\n");
    lr_log(RETRO_LOG_INFO, "  Backend: %s\n", cfg.backend_renderer.c_str());
    lr_log(RETRO_LOG_INFO, "  Resolution: %.1fx\n", cfg.resolution_multiplier);
    lr_log(RETRO_LOG_INFO, "  Screen Filter: %s\n", cfg.screen_filter.c_str());
    lr_log(RETRO_LOG_INFO, "  Surface Sync: %s\n", cfg.disable_surface_sync ? "disabled" : "enabled");
    lr_log(RETRO_LOG_INFO, "  FPS Hack: %s\n", cfg.fps_hack ? "enabled" : "disabled");
    lr_log(RETRO_LOG_INFO, "  Memory Mapping: %s\n", cfg.memory_mapping.c_str());
    lr_log(RETRO_LOG_INFO, "  CPU Opt: %s\n", cfg.cpu_opt ? "enabled" : "disabled");
    lr_log(RETRO_LOG_INFO, "  Audio Volume: %d%%\n", cfg.audio_volume);
}

static void read_core_options() {
    if (!libretro.cfg) return;
    
    // GPU Options
    read_core_option("vita3k_backend_renderer", "Vulkan", [](const char *value) {
        const std::string token = normalize_option_token(value);
        libretro.cfg->current_config.backend_renderer = (token == "opengl") ? "OpenGL" : "Vulkan";
    });
    
    read_core_option("vita3k_resolution_multiplier", "1", [](const char *value) {
        libretro.cfg->current_config.resolution_multiplier = parse_float_option(value, 1.0f);
    });
    
    read_core_option("vita3k_screen_filter", "Bilinear", [](const char *value) {
        libretro.cfg->current_config.screen_filter = value;
    });
    
    read_core_option("vita3k_anisotropic_filtering", "1", [](const char *value) {
        libretro.cfg->current_config.anisotropic_filtering = parse_int_option(value, 1);
    });
    
    read_core_option("vita3k_v_sync", "enabled", [](const char *value) {
        libretro.cfg->current_config.v_sync = parse_enabled_option(value, true);
    });
    
    read_core_option("vita3k_async_pipeline", "enabled", [](const char *value) {
        libretro.cfg->current_config.async_pipeline_compilation = parse_enabled_option(value, true);
    });
    
    read_core_option("vita3k_surface_sync", "enabled", [](const char *value) {
        // Inverted logic: disable_surface_sync = !surface_sync_enabled
        libretro.cfg->current_config.disable_surface_sync = !parse_enabled_option(value, true);
    });
    
    read_core_option("vita3k_high_accuracy", "disabled", [](const char *value) {
        libretro.cfg->current_config.high_accuracy = parse_enabled_option(value, false);
    });
    
    read_core_option("vita3k_memory_mapping", "double-buffer", [](const char *value) {
        libretro.cfg->current_config.memory_mapping = canonical_memory_mapping_option(value);
    });
    
    // System Options
    read_core_option("vita3k_pstv_mode", "disabled", [](const char *value) {
        libretro.cfg->current_config.pstv_mode = parse_enabled_option(value, false);
    });
    
    read_core_option("vita3k_sys_button", "1", [](const char *value) {
        libretro.cfg->sys_button = parse_int_option(value, 1);
    });
    
    read_core_option("vita3k_modules_mode", "0", [](const char *value) {
        int mode = parse_int_option(value, 0);
        if (mode < ModulesMode::AUTOMATIC)
            mode = ModulesMode::AUTOMATIC;
        if (mode > ModulesMode::MANUAL)
            mode = ModulesMode::MANUAL;
        libretro.cfg->current_config.modules_mode = mode;
    });
    
    read_core_option("vita3k_fps_hack", "disabled", [](const char *value) {
        libretro.cfg->current_config.fps_hack = parse_enabled_option(value, false);
    });
    
    read_core_option("vita3k_file_loading_delay", "0", [](const char *value) {
        int delay = parse_int_option(value, 0);
        if (delay < 0)
            delay = 0;
        if (delay > 30)
            delay = 30;
        libretro.cfg->current_config.file_loading_delay = delay;
    });
    
    read_core_option("vita3k_touchpad_cursor", "enabled", [](const char *value) {
        libretro.cfg->current_config.show_touchpad_cursor = parse_enabled_option(value, true);
    });
    
    // Audio Options
    read_core_option("vita3k_audio_volume", "100", [](const char *value) {
        int volume = parse_int_option(value, 100);
        if (volume < 0)
            volume = 0;
        if (volume > 100)
            volume = 100;
        libretro.cfg->current_config.audio_volume = volume;
    });
    
    read_core_option("vita3k_ngs_enable", "enabled", [](const char *value) {
        libretro.cfg->current_config.ngs_enable = parse_enabled_option(value, true);
    });
    
    // CPU Options
    read_core_option("vita3k_cpu_opt", "enabled", [](const char *value) {
        libretro.cfg->current_config.cpu_opt = parse_enabled_option(value, true);
    });
    
    // Network Options
    read_core_option("vita3k_psn_signed_in", "disabled", [](const char *value) {
        libretro.cfg->current_config.psn_signed_in = parse_enabled_option(value, false);
    });
}

RETRO_API size_t retro_serialize_size(void) {
    return 0; // Save states not supported
}

RETRO_API bool retro_serialize(void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

RETRO_API void retro_reset(void) {
    lr_log(RETRO_LOG_INFO,
        "retro_reset requested (game_loaded=%d, emuenv=%d) -> scheduling deferred reset\n",
        libretro.game_loaded ? 1 : 0,
        libretro.emuenv ? 1 : 0);
    libretro.reset_requested = true;
}

RETRO_API void retro_cheat_reset(void) {}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index;
    (void)enabled;
    (void)code;
}

RETRO_API unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id) {
    (void)id;
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    (void)id;
    return 0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    const char *name = "Unknown";
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_NONE: name = "None"; break;
    case RETRO_DEVICE_JOYPAD: name = "Joypad"; break;
    case RETRO_DEVICE_ANALOG: name = "Analog"; break;
    case RETRO_DEVICE_POINTER: name = "Pointer"; break;
    }
    lr_log(RETRO_LOG_INFO, "Set controller port %u device %u (%s)\n", port, device, name);
}
