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

#include <cstdint>
#include <mutex>

static constexpr uint32_t LIBRETRO_MAX_PORTS = 4;

static constexpr uint32_t LIBRETRO_MAX_TOUCH_POINTS = 8;

struct LibretroPortState {
    uint32_t buttons = 0;
    uint8_t lx = 0x80;
    uint8_t ly = 0x80;
    uint8_t rx = 0x80;
    uint8_t ry = 0x80;
};

struct LibretroInputState {
    std::mutex mutex;

    LibretroPortState ports[LIBRETRO_MAX_PORTS] = {};

    struct TouchPoint {
        int16_t x = 0;
        int16_t y = 0;
        uint8_t id = 0;
        bool active = false;
    };

    TouchPoint front_touch[LIBRETRO_MAX_TOUCH_POINTS] = {};
    uint32_t front_touch_count = 0;

    TouchPoint back_touch[LIBRETRO_MAX_TOUCH_POINTS] = {};
    uint32_t back_touch_count = 0;

    float accel_x = 0.0f, accel_y = 0.0f, accel_z = -1.0f;
    float gyro_x = 0.0f, gyro_y = 0.0f, gyro_z = 0.0f;
    bool has_motion = false;
    bool motion_enabled = false;
    bool supports_bitmasks = false;
};

extern LibretroInputState &libretro_input_state();
