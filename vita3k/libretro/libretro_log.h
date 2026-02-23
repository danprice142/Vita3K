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

#include "libretro_state.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/null_mutex.h>

#include <mutex>

// Custom spdlog sink that routes log messages to the libretro frontend
// log callback. This allows all Vita3K internal logging (LOG_INFO,
// LOG_ERROR, etc.) to appear in the RetroArch log.

template <typename Mutex>
class libretro_sink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        if (!libretro.log_cb)
            return;

        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        std::string text = fmt::to_string(formatted);

        // Strip trailing newline if present (libretro adds its own)
        if (!text.empty() && text.back() == '\n')
            text.pop_back();

        enum retro_log_level level;
        switch (msg.level) {
        case spdlog::level::trace:
        case spdlog::level::debug:
            level = RETRO_LOG_DEBUG;
            break;
        case spdlog::level::info:
            level = RETRO_LOG_INFO;
            break;
        case spdlog::level::warn:
            level = RETRO_LOG_WARN;
            break;
        case spdlog::level::err:
        case spdlog::level::critical:
            level = RETRO_LOG_ERROR;
            break;
        default:
            level = RETRO_LOG_INFO;
            break;
        }

        libretro.log_cb(level, "[Vita3K] %s\n", text.c_str());
    }

    void flush_() override {
        // No-op — libretro frontend handles flushing
    }
};

using libretro_sink_mt = libretro_sink<std::mutex>;
using libretro_sink_st = libretro_sink<spdlog::details::null_mutex>;

// Initialize Vita3K's spdlog to route through libretro log callback.
// Call this after the libretro log callback is set up.
void libretro_logging_init();
