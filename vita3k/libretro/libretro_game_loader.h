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

#include <util/exit_code.h>

#include <cstdint>

struct EmuEnvState;
class Root;

// Initialize the emulator environment: paths, renderer, IO, memory, audio, NGS.
// This calls app::init() + app::late_init() in sequence.
bool libretro_init_emuenv(EmuEnvState &emuenv, const Root &root_paths);

// Load the game binary (eboot.bin + preload modules) into the emulator.
// Equivalent to standalone load_app().
ExitCode libretro_load_app(int32_t &main_module_id, EmuEnvState &emuenv);

// Start the game's main thread.
// Equivalent to standalone run_app().
ExitCode libretro_run_app(EmuEnvState &emuenv, int32_t main_module_id);
