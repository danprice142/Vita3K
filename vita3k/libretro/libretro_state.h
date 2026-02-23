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

#pragma once

#include "libretro.h"
#include "libretro_vulkan.h"
#include "libretro_input_state.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct EmuEnvState;
struct Config;
struct Root;

static constexpr uint32_t LIBRETRO_MAX_SWAPCHAIN = 8;

struct LibretroVulkanPresentation {
    const struct retro_hw_render_interface_vulkan *vulkan = nullptr;

    uint32_t num_images = 0;
    struct retro_vulkan_image images[LIBRETRO_MAX_SWAPCHAIN] = {};
    VkImage vk_images[LIBRETRO_MAX_SWAPCHAIN] = {};
    VkImageView vk_views[LIBRETRO_MAX_SWAPCHAIN] = {};
    VkDeviceMemory vk_memory[LIBRETRO_MAX_SWAPCHAIN] = {};

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buffers[LIBRETRO_MAX_SWAPCHAIN] = {};

    uint32_t width = 960;
    uint32_t height = 544;
};

struct LibretroState {
    retro_environment_t environ_cb = nullptr;
    retro_video_refresh_t video_cb = nullptr;
    retro_audio_sample_t audio_sample_cb = nullptr;
    retro_audio_sample_batch_t audio_batch_cb = nullptr;
    retro_input_poll_t input_poll_cb = nullptr;
    retro_input_state_t input_state_cb = nullptr;
    retro_log_printf_t log_cb = nullptr;

    std::unique_ptr<EmuEnvState> emuenv;
    std::unique_ptr<Config> cfg;
    std::unique_ptr<Root> root_paths;

    std::mutex audio_mutex;
    std::vector<int16_t> audio_buffer;

    std::string system_dir;
    std::string save_dir;
    std::string content_dir;

    std::string game_path;
    std::string installed_title_id;
    std::string installed_title;

    LibretroInputState input;

    bool initialized = false;
    bool game_loaded = false;
    bool renderer_ready = false;
    bool app_started = false;
    std::atomic<bool> reset_requested{false};
    int32_t pending_main_module_id = -1;

    std::unique_ptr<std::thread> render_thread;
    std::atomic<bool> render_thread_running{false};

    std::mutex rendered_frame_mutex;
    std::atomic<bool> has_new_frame{false};

    VkImage vk_surface_image = VK_NULL_HANDLE;
    uint32_t vk_surface_width = 0;
    uint32_t vk_surface_height = 0;
    uint32_t vk_surface_offset_x = 0;
    uint32_t vk_surface_offset_y = 0;

    retro_hw_render_callback hw_render = {};
    enum retro_hw_context_type active_context = RETRO_HW_CONTEXT_NONE;

    LibretroVulkanPresentation vk_presentation;

    uint32_t gl_screen_texture = 0;

    std::vector<uint32_t> frame_buffer;
};

extern LibretroState libretro;
