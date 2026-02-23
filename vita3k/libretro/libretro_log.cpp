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

#include "libretro_log.h"
#include "libretro_state.h"

#include <spdlog/spdlog.h>

#include <vector>

void libretro_logging_init() {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<libretro_sink_mt>());

    auto logger = std::make_shared<spdlog::logger>("vita3k logger", sinks.begin(), sinks.end());
    logger->set_pattern("%^[%H:%M:%S.%e] |%L| [%!]: %v%$");
    logger->set_level(spdlog::level::trace);

    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::trace);

    if (libretro.log_cb)
        libretro.log_cb(RETRO_LOG_INFO, "[Vita3K] spdlog -> libretro log sink initialized\n");
}
