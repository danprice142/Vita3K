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

#include "libretro_input.h"
#include "libretro_state.h"

#include <ctrl/ctrl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

extern void lr_log(enum retro_log_level level, const char *fmt, ...);

static struct retro_sensor_interface sensor_iface = {};
struct ButtonMapping {
    unsigned retro_id;
    uint32_t vita_button;
};

static constexpr ButtonMapping button_map[] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,     SCE_CTRL_UP },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   SCE_CTRL_DOWN },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   SCE_CTRL_LEFT },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  SCE_CTRL_RIGHT },
    { RETRO_DEVICE_ID_JOYPAD_B,      SCE_CTRL_CROSS },
    { RETRO_DEVICE_ID_JOYPAD_A,      SCE_CTRL_CIRCLE },
    { RETRO_DEVICE_ID_JOYPAD_Y,      SCE_CTRL_SQUARE },
    { RETRO_DEVICE_ID_JOYPAD_X,      SCE_CTRL_TRIANGLE },
    { RETRO_DEVICE_ID_JOYPAD_L,      SCE_CTRL_L | SCE_CTRL_L1 },
    { RETRO_DEVICE_ID_JOYPAD_R,      SCE_CTRL_R | SCE_CTRL_R1 },
    { RETRO_DEVICE_ID_JOYPAD_L2,     SCE_CTRL_L2 },
    { RETRO_DEVICE_ID_JOYPAD_R2,     SCE_CTRL_R2 },
    { RETRO_DEVICE_ID_JOYPAD_L3,     SCE_CTRL_L3 },
    { RETRO_DEVICE_ID_JOYPAD_R3,     SCE_CTRL_R3 },
    { RETRO_DEVICE_ID_JOYPAD_SELECT, SCE_CTRL_SELECT },
    { RETRO_DEVICE_ID_JOYPAD_START,  SCE_CTRL_START },
};
static constexpr int BUTTON_MAP_COUNT = sizeof(button_map) / sizeof(button_map[0]);

static uint8_t axis_to_vita(int16_t val) {
    int shifted = static_cast<int>(val) + 0x8000;
    shifted = std::clamp(shifted, 0, 0xFFFF);
    return static_cast<uint8_t>(shifted >> 8);
}

static void poll_port(unsigned port, LibretroPortState &out, bool supports_bitmasks) {
    uint32_t buttons = 0;

    if (supports_bitmasks) {
        int16_t bitmask = libretro.input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        for (int i = 0; i < BUTTON_MAP_COUNT; i++) {
            if (bitmask & (1 << button_map[i].retro_id))
                buttons |= button_map[i].vita_button;
        }
    } else {
        for (int i = 0; i < BUTTON_MAP_COUNT; i++) {
            if (libretro.input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, button_map[i].retro_id))
                buttons |= button_map[i].vita_button;
        }
    }

    out.buttons = buttons;
    out.lx = axis_to_vita(libretro.input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X));
    out.ly = axis_to_vita(libretro.input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y));
    out.rx = axis_to_vita(libretro.input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
    out.ry = axis_to_vita(libretro.input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
}

static void add_port_descriptors(std::vector<retro_input_descriptor> &desc, unsigned port) {
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L Trigger" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R Trigger" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2 Trigger" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2 Trigger" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" });
    desc.push_back({ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" });
    desc.push_back({ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" });
    desc.push_back({ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" });
    desc.push_back({ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" });
    desc.push_back({ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" });
}

void libretro_init_input_descriptors() {
    std::vector<retro_input_descriptor> desc;

    for (unsigned p = 0; p < LIBRETRO_MAX_PORTS; p++)
        add_port_descriptors(desc, p);

    desc.push_back({ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X, "Touch X" });
    desc.push_back({ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y, "Touch Y" });
    desc.push_back({ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED, "Touch Pressed" });
    desc.push_back({ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT, "Touch Count" });

    desc.push_back({ 0, 0, 0, 0, NULL });

    libretro.environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc.data());
}

void libretro_setup_controller_info() {
    static const struct retro_controller_description ctrl_types[] = {
        { "PS Vita Controller", RETRO_DEVICE_JOYPAD },
        { "PS Vita Controller (Analog)", RETRO_DEVICE_ANALOG },
    };
    static const struct retro_controller_info ports[] = {
        { ctrl_types, 2 },
        { ctrl_types, 2 },
        { ctrl_types, 2 },
        { ctrl_types, 2 },
        { NULL, 0 },
    };
    libretro.environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);

    bool bitmask_supported = false;
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &bitmask_supported)) {
        libretro.input.supports_bitmasks = bitmask_supported;
    }

    memset(&sensor_iface, 0, sizeof(sensor_iface));
    if (libretro.environ_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensor_iface)
        && sensor_iface.set_sensor_state && sensor_iface.get_sensor_input) {
        libretro.input.has_motion = true;
        sensor_iface.set_sensor_state(0, RETRO_SENSOR_ACCELEROMETER_ENABLE, 60);
        sensor_iface.set_sensor_state(0, RETRO_SENSOR_GYROSCOPE_ENABLE, 60);
        libretro.input.motion_enabled = true;
    } else {
        libretro.input.has_motion = false;
    }
}

void libretro_poll_input() {
    if (!libretro.input_state_cb)
        return;

    auto &inp = libretro.input;

    LibretroPortState port_data[LIBRETRO_MAX_PORTS] = {};
    for (unsigned p = 0; p < LIBRETRO_MAX_PORTS; p++)
        poll_port(p, port_data[p], inp.supports_bitmasks);

    // L2 = rear touchpad modifier
    bool rear_modifier = !!(port_data[0].buttons & SCE_CTRL_L2);

    uint32_t touch_count = 0;
    LibretroInputState::TouchPoint touch_points[LIBRETRO_MAX_TOUCH_POINTS] = {};

    for (uint32_t idx = 0; idx < LIBRETRO_MAX_TOUCH_POINTS; idx++) {
        int16_t pressed = libretro.input_state_cb(0, RETRO_DEVICE_POINTER, idx, RETRO_DEVICE_ID_POINTER_PRESSED);
        if (!pressed)
            break;

        int16_t ptr_x = libretro.input_state_cb(0, RETRO_DEVICE_POINTER, idx, RETRO_DEVICE_ID_POINTER_X);
        int16_t ptr_y = libretro.input_state_cb(0, RETRO_DEVICE_POINTER, idx, RETRO_DEVICE_ID_POINTER_Y);

        float norm_x = (static_cast<float>(ptr_x) + 0x7FFF) / (2.0f * 0x7FFF);
        float norm_y = (static_cast<float>(ptr_y) + 0x7FFF) / (2.0f * 0x7FFF);

        if (rear_modifier) {
            touch_points[touch_count].x = static_cast<int16_t>(std::clamp(norm_x * 1920.0f, 0.0f, 1919.0f));
            touch_points[touch_count].y = static_cast<int16_t>(std::clamp(108.0f + norm_y * 781.0f, 108.0f, 889.0f));
        } else {
            touch_points[touch_count].x = static_cast<int16_t>(std::clamp(norm_x * 1920.0f, 0.0f, 1919.0f));
            touch_points[touch_count].y = static_cast<int16_t>(std::clamp(norm_y * 1088.0f, 0.0f, 1087.0f));
        }
        touch_points[touch_count].id = static_cast<uint8_t>(idx % 128);
        touch_points[touch_count].active = true;
        touch_count++;
    }

    if ((port_data[0].buttons & SCE_CTRL_CROSS) && touch_count == 0 && !rear_modifier) {
        touch_points[0].x = 960;
        touch_points[0].y = 544;
        touch_points[0].id = 99;
        touch_points[0].active = true;
        touch_count = 1;
    }

    float ax = 0.0f, ay = 0.0f, az = -1.0f;
    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    if (inp.has_motion && inp.motion_enabled && sensor_iface.get_sensor_input) {
        float raw_ax = sensor_iface.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_X);
        float raw_ay = sensor_iface.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Y);
        float raw_az = sensor_iface.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Z);
        float raw_gx = sensor_iface.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_X);
        float raw_gy = sensor_iface.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_Y);
        float raw_gz = sensor_iface.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_Z);

        constexpr float GRAVITY = 9.80665f;
        ax = raw_ax / -GRAVITY;
        ay = raw_ay / -GRAVITY;
        az = raw_az / -GRAVITY;

        constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;
        gx = raw_gx / TWO_PI;
        gy = raw_gy / TWO_PI;
        gz = raw_gz / TWO_PI;
    }

    {
        std::lock_guard<std::mutex> lock(inp.mutex);

        memcpy(inp.ports, port_data, sizeof(port_data));

        if (rear_modifier) {
            inp.front_touch_count = 0;
            memcpy(inp.back_touch, touch_points, sizeof(touch_points));
            inp.back_touch_count = touch_count;
        } else {
            memcpy(inp.front_touch, touch_points, sizeof(touch_points));
            inp.front_touch_count = touch_count;
            inp.back_touch_count = 0;
        }

        inp.accel_x = ax;
        inp.accel_y = ay;
        inp.accel_z = az;
        inp.gyro_x = gx;
        inp.gyro_y = gy;
        inp.gyro_z = gz;
    }
}
