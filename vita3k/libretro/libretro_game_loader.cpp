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

#include "libretro_game_loader.h"
#include "libretro_state.h"

#include <app/functions.h>
#include <config/state.h>
#include <display/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <io/vfs.h>
#include <kernel/state.h>
#include <modules/module_parent.h>
#include <util/log.h>
#include <util/tracy.h>
#include <packages/license.h>
#include <packages/sfo.h>
#include <renderer/state.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <module/load_module.h>

bool libretro_init_emuenv(EmuEnvState &emuenv, const Root &root_paths) {
    LOG_INFO("=== Initializing EmuEnvState (libretro) ===");

    // app::init sets up paths, renderer, and IO
    if (!app::init(emuenv, *libretro.cfg, root_paths)) {
        LOG_ERROR("app::init failed!");
        return false;
    }
    LOG_INFO("app::init succeeded");

    // Set the app path to the installed title_id
    emuenv.io.app_path = libretro.installed_title_id;
    emuenv.io.title_id = libretro.installed_title_id;
    emuenv.current_app_title = libretro.installed_title;
    emuenv.app_info.app_title_id = libretro.installed_title_id;
    emuenv.app_info.app_title = libretro.installed_title;

    // Read param.sfo for additional app info
    vfs::FileBuffer param_sfo;
    if (vfs::read_app_file(param_sfo, emuenv.pref_path, emuenv.io.app_path, "sce_sys/param.sfo")) {
        sfo::load(emuenv.sfo_handle, param_sfo);
        sfo::SfoAppInfo app_info;
        sfo::get_param_info(app_info, param_sfo, 1);
        if (!app_info.app_version.empty())
            emuenv.app_info.app_version = app_info.app_version;
        if (!app_info.app_category.empty())
            emuenv.app_info.app_category = app_info.app_category;
        if (!app_info.app_content_id.empty())
            emuenv.io.content_id = app_info.app_content_id;
        LOG_INFO("param.sfo loaded: title='{}' id='{}' ver='{}' cat='{}'",
            emuenv.app_info.app_title, emuenv.app_info.app_title_id,
            emuenv.app_info.app_version, emuenv.app_info.app_category);
    } else {
        LOG_WARN("Could not read param.sfo for {}", emuenv.io.app_path);
    }

    // Load license key for FSELF decryption (NoNpDrm games)
    get_license(emuenv, emuenv.io.title_id, emuenv.io.content_id);
    LOG_INFO("License check done for {} (content_id: {})", emuenv.io.title_id, emuenv.io.content_id);

    // Initialize libraries (HLE module imports)
    init_libraries(emuenv);
    LOG_INFO("HLE libraries initialized");

    // app::late_init sets up memory, audio, NGS
    if (!app::late_init(emuenv)) {
        LOG_ERROR("app::late_init failed!");
        return false;
    }
    LOG_INFO("app::late_init succeeded (memory, audio, NGS ready)");

    return true;
}

ExitCode libretro_load_app(int32_t &main_module_id, EmuEnvState &emuenv) {
    LOG_INFO("=== Loading app (libretro) ===");
    LOG_INFO("Title: {}", emuenv.current_app_title);
    LOG_INFO("Serial: {}", emuenv.io.title_id);
    LOG_INFO("Version: {}", emuenv.app_info.app_version);
    LOG_INFO("Category: {}", emuenv.app_info.app_category);

    const auto call_import = [&emuenv](CPUState &cpu, uint32_t nid, SceUID thread_id) {
        ::call_import(emuenv, cpu, nid, thread_id);
    };
    if (!emuenv.kernel.init(emuenv.mem, call_import, emuenv.kernel.cpu_opt)) {
        LOG_ERROR("Failed to init kernel!");
        return KernelInitFailed;
    }
    LOG_INFO("Kernel initialized");

    LOG_INFO("CPU Optimisation state: {}", emuenv.cfg.current_config.cpu_opt);
    LOG_INFO("ngs state: {}", emuenv.cfg.current_config.ngs_enable);
    LOG_INFO("Resolution multiplier: {}", emuenv.cfg.resolution_multiplier);

    init_device_paths(emuenv.io);
    init_savedata_app_path(emuenv.io, emuenv.pref_path);
    LOG_INFO("Device paths and savedata initialized");

    // Reload param.sfo via VFS
    vfs::FileBuffer param_sfo;
    if (vfs::read_app_file(param_sfo, emuenv.pref_path, emuenv.io.app_path, "sce_sys/param.sfo"))
        sfo::load(emuenv.sfo_handle, param_sfo);

    init_exported_vars(emuenv);
    LOG_INFO("Exported vars initialized");

    // Load main executable
    emuenv.self_path = !emuenv.cfg.self_path.empty() ? emuenv.cfg.self_path : EBOOT_PATH;
    LOG_INFO("Loading main executable: app0:{}", emuenv.self_path);
    main_module_id = load_module(emuenv, "app0:" + emuenv.self_path);
    if (main_module_id >= 0) {
        const auto module = emuenv.kernel.loaded_modules[main_module_id];
        LOG_INFO("Main executable {} ({}) loaded successfully", module->info.module_name, emuenv.self_path);
    } else {
        LOG_ERROR("Failed to load main executable: app0:{}", emuenv.self_path);
        return FileNotFound;
    }
    emuenv.self_name = fs::path(emuenv.self_path).filename().string();

    // Load preload modules
    SceUInt32 process_preload_disabled = 0;
    auto process_param = emuenv.kernel.process_param.get(emuenv.mem);
    if (process_param) {
        auto preload_disabled_ptr = Ptr<SceUInt32>(process_param->process_preload_disabled);
        if (preload_disabled_ptr) {
            process_preload_disabled = *preload_disabled_ptr.get(emuenv.mem);
        }
    }
    const auto module_app_path{ emuenv.pref_path / "ux0/app" / emuenv.io.app_path / "sce_module" };

    std::vector<std::string> lib_load_list = {};
    auto add_preload_module = [&](uint32_t code, SceSysmoduleModuleId module_id, const std::string &name, bool load_from_app) {
        if ((process_preload_disabled & code) == 0) {
            if (is_lle_module(name, emuenv)) {
                const auto module_name_file = fmt::format("{}.suprx", name);
                if (load_from_app && fs::exists(module_app_path / module_name_file))
                    lib_load_list.emplace_back(fmt::format("app0:sce_module/{}", module_name_file));
                else if (fs::exists(emuenv.pref_path / "vs0/sys/external" / module_name_file))
                    lib_load_list.emplace_back(fmt::format("vs0:sys/external/{}", module_name_file));
            }

            if (module_id != SCE_SYSMODULE_INVALID)
                emuenv.kernel.loaded_sysmodules[module_id] = {};
        }
    };
    add_preload_module(0x00010000, SCE_SYSMODULE_INVALID, "libc", true);
    add_preload_module(0x00020000, SCE_SYSMODULE_DBG, "libdbg", false);
    add_preload_module(0x00080000, SCE_SYSMODULE_INVALID, "libshellsvc", false);
    add_preload_module(0x00100000, SCE_SYSMODULE_INVALID, "libcdlg", false);
    add_preload_module(0x00200000, SCE_SYSMODULE_FIOS2, "libfios2", true);
    add_preload_module(0x00400000, SCE_SYSMODULE_APPUTIL, "apputil", false);
    add_preload_module(0x00800000, SCE_SYSMODULE_INVALID, "libSceFt2", false);
    add_preload_module(0x01000000, SCE_SYSMODULE_INVALID, "libpvf", false);
    add_preload_module(0x02000000, SCE_SYSMODULE_PERF, "libperf", false);

    for (const auto &module_path : lib_load_list) {
        LOG_INFO("Loading preload module: {}", module_path);
        auto res = load_module(emuenv, module_path);
        if (res < 0) {
            LOG_ERROR("Failed to load preload module: {}", module_path);
            return FileNotFound;
        }
    }

    LOG_INFO("All modules loaded successfully");

    if (!emuenv.cfg.show_gui)
        emuenv.display.imgui_render = false;

    // Set renderer app context for shader cache (if renderer exists)
    if (emuenv.renderer)
        emuenv.renderer->set_app(emuenv.io.title_id.c_str(), emuenv.self_name.c_str());

    return Success;
}

ExitCode libretro_run_app(EmuEnvState &emuenv, int32_t main_module_id) {
    LOG_INFO("=== Starting app (libretro) ===");

    auto entry_point = emuenv.kernel.loaded_modules[main_module_id]->info.start_entry;
    auto process_param = emuenv.kernel.process_param.get(emuenv.mem);

    SceInt32 priority = SCE_KERNEL_DEFAULT_PRIORITY_USER;
    SceInt32 stack_size = SCE_KERNEL_STACK_SIZE_USER_MAIN;
    SceInt32 affinity = SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
    if (process_param) {
        auto priority_ptr = Ptr<int32_t>(process_param->main_thread_priority);
        if (priority_ptr)
            priority = *priority_ptr.get(emuenv.mem);

        auto stack_size_ptr = Ptr<int32_t>(process_param->main_thread_stacksize);
        if (stack_size_ptr)
            stack_size = *stack_size_ptr.get(emuenv.mem);

        auto affinity_ptr = Ptr<SceInt32>(process_param->main_thread_cpu_affinity_mask);
        if (affinity_ptr)
            affinity = *affinity_ptr.get(emuenv.mem);
    }

    LOG_INFO("Creating main thread: priority={} stack_size={} affinity={}", priority, stack_size, affinity);

    const ThreadStatePtr main_thread = emuenv.kernel.create_thread(emuenv.mem, emuenv.io.title_id.c_str(), entry_point, priority, affinity, stack_size, nullptr);
    if (!main_thread) {
        LOG_ERROR("Failed to create main thread!");
        return InitThreadFailed;
    }
    emuenv.main_thread_id = main_thread->id;
    LOG_INFO("Main thread created: id={}", main_thread->id);

    // Run module_start for all loaded libraries
    for (auto &[_, module] : emuenv.kernel.loaded_modules) {
        if (module->info.modid != main_module_id) {
            start_module(emuenv, module->info);
        }
    }
    LOG_INFO("All module_start entries executed");

    SceKernelThreadOptParam param{ 0, 0 };
    if (!emuenv.cfg.app_args.empty()) {
        LOG_INFO("App args: {}", emuenv.cfg.app_args);
    }

    if (main_thread->start(param.size, Ptr<void>(param.attr), true) < 0) {
        LOG_ERROR("Failed to start main thread!");
        return RunThreadFailed;
    }
    LOG_INFO("Main thread started!");

    start_sync_thread(emuenv);
    LOG_INFO("Sync thread started");

    return Success;
}
