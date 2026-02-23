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
#include "libretro_state.h"

static struct retro_core_option_v2_category option_categories[] = {
    { "gpu", "GPU", "Graphics rendering options." },
    { "cpu", "CPU", "CPU emulation options." },
    { "system", "System", "System emulation options." },
    { "audio", "Audio", "Audio output options." },
    { "network", "Network", "Network options." },
    { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_definitions[] = {
    // ── GPU ──────────────────────────────────────────────────────────────
    {
        "vita3k_backend_renderer",
        "Backend Renderer",
        NULL,
        "Select the graphics backend. Vulkan is recommended.",
        NULL,
        "gpu",
        {
            { "Vulkan", "Vulkan" },
            { "OpenGL", "OpenGL" },
            { NULL, NULL },
        },
        "Vulkan"
    },
    {
        "vita3k_resolution_multiplier",
        "Internal Resolution Multiplier",
        NULL,
        "Multiplier for the internal rendering resolution (960x544 base).",
        NULL,
        "gpu",
        {
            { "1", "1x (960x544)" },
            { "2", "2x (1920x1088)" },
            { "3", "3x (2880x1632)" },
            { "4", "4x (3840x2176)" },
            { NULL, NULL },
        },
        "1"
    },
    {
        "vita3k_screen_filter",
        "Screen Filter",
        NULL,
        "Post-processing filter applied to the rendered frame.",
        NULL,
        "gpu",
        {
            { "Bilinear", "Bilinear" },
            { "Nearest", "Nearest" },
            { "Bicubic", "Bicubic" },
            { "FXAA", "FXAA" },
            { "FSR", "FSR" },
            { NULL, NULL },
        },
        "Bilinear"
    },
    {
        "vita3k_anisotropic_filtering",
        "Anisotropic Filtering",
        NULL,
        "Level of anisotropic texture filtering.",
        NULL,
        "gpu",
        {
            { "1", "1x (Off)" },
            { "2", "2x" },
            { "4", "4x" },
            { "8", "8x" },
            { "16", "16x" },
            { NULL, NULL },
        },
        "1"
    },
    {
        "vita3k_v_sync",
        "V-Sync",
        NULL,
        "Enable or disable vertical sync.",
        NULL,
        "gpu",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "vita3k_async_pipeline",
        "Async Pipeline Compilation",
        NULL,
        "Compile shader pipelines asynchronously to reduce stuttering.",
        NULL,
        "gpu",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "vita3k_memory_mapping",
        "Memory Mapping",
        NULL,
        "GPU memory mapping method. Higher methods improve performance but require GPU support.",
        NULL,
        "gpu",
        {
            { "disabled", "Disabled" },
            { "double-buffer", "Double Buffer" },
            { "external-host", "External Host" },
            { "page-table", "Page Table" },
            { "native-buffer", "Native Buffer" },
            { NULL, NULL },
        },
        "double-buffer"
    },
    {
        "vita3k_high_accuracy",
        "High Accuracy Rendering",
        NULL,
        "Enable more accurate but slower rendering techniques.",
        NULL,
        "gpu",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    // ── CPU ──────────────────────────────────────────────────────────────
    {
        "vita3k_cpu_opt",
        "CPU Optimizations (Dynarmic JIT)",
        NULL,
        "Enable Dynarmic JIT for faster CPU emulation.",
        NULL,
        "cpu",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    // ── System ───────────────────────────────────────────────────────────
    {
        "vita3k_pstv_mode",
        "PS TV Mode",
        NULL,
        "Emulate a PlayStation TV instead of a PS Vita.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "vita3k_modules_mode",
        "Modules Mode",
        NULL,
        "How system modules are loaded. Automatic is recommended.",
        NULL,
        "system",
        {
            { "0", "Automatic" },
            { "1", "Auto & Manual" },
            { "2", "Manual" },
            { NULL, NULL },
        },
        "0"
    },
    {
        "vita3k_fps_hack",
        "FPS Hack (30 to 60)",
        NULL,
        "Attempt to run 30fps-locked games at 60fps. May cause issues.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "vita3k_sys_button",
        "Confirm Button",
        NULL,
        "Which button is used for confirm actions.",
        NULL,
        "system",
        {
            { "0", "Circle" },
            { "1", "Cross" },
            { NULL, NULL },
        },
        "1"
    },
    // ── Audio ────────────────────────────────────────────────────────────
    {
        "vita3k_audio_volume",
        "Audio Volume",
        NULL,
        "Master audio volume level.",
        NULL,
        "audio",
        {
            { "0", "0%" },
            { "10", "10%" },
            { "20", "20%" },
            { "30", "30%" },
            { "40", "40%" },
            { "50", "50%" },
            { "60", "60%" },
            { "70", "70%" },
            { "80", "80%" },
            { "90", "90%" },
            { "100", "100%" },
            { NULL, NULL },
        },
        "100"
    },
    {
        "vita3k_ngs_enable",
        "NGS Audio Engine",
        NULL,
        "Enable the NGS audio engine for enhanced audio.",
        NULL,
        "audio",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    // ── Network ──────────────────────────────────────────────────────────
    {
        "vita3k_psn_signed_in",
        "PSN Signed In",
        NULL,
        "Pretend to be signed in to PSN. Required by some games.",
        NULL,
        "network",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "vita3k_surface_sync",
        "Surface Sync",
        NULL,
        "GPU memory optimization. May improve performance by 20-40%.",
        NULL,
        "gpu",
        {
            { "enabled", "Enabled (more accurate)" },
            { "disabled", "Disabled (faster)" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "vita3k_file_loading_delay",
        "File Loading Delay",
        NULL,
        "Delay in milliseconds for file loading. Helps with timing-sensitive games.",
        NULL,
        "system",
        {
            { "0", "0 ms" },
            { "5", "5 ms" },
            { "10", "10 ms" },
            { "15", "15 ms" },
            { "20", "20 ms" },
            { "25", "25 ms" },
            { "30", "30 ms" },
            { NULL, NULL },
        },
        "0"
    },
    {
        "vita3k_touchpad_cursor",
        "Touchpad Cursor",
        NULL,
        "Show touchpad cursor overlay for accessibility.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

static struct retro_core_options_v2 options_v2 = {
    option_categories,
    option_definitions,
};

static inline void libretro_set_core_options() {
    unsigned version = 0;
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        libretro.environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
    } else if (version >= 1) {
        // Fallback to v1 — just set the v2 struct, many frontends handle it
        libretro.environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
    }
}
