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

#include "audio/impl/libretro_audio.h"

#include "kernel/thread/thread_state.h"

#include "util/log.h"

#include <algorithm>
#include <cstring>

static constexpr int NUM_AUDIO_BUFFERS = 8;

LibretroAudioAdapter::LibretroAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

bool LibretroAudioAdapter::init() {
    LOG_INFO("LibretroAudioAdapter initialized");
    return true;
}

AudioOutPortPtr LibretroAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    auto port = std::make_shared<LibretroAudioOutPort>();
    port->channels = nb_channels;
    port->freq = freq;
    port->len = nb_sample;
    port->len_bytes = nb_sample * nb_channels * sizeof(int16_t);
    port->len_microseconds = (static_cast<uint64_t>(nb_sample) * 1'000'000ULL) / freq;

    port->audio_buffers.resize(NUM_AUDIO_BUFFERS);
    for (auto &buf : port->audio_buffers) {
        buf.resize(port->len_bytes, 0);
    }

    LOG_INFO("LibretroAudioAdapter: opened port ch={} freq={} samples={} buf_size={}",
        nb_channels, freq, nb_sample, port->len_bytes);
    return port;
}

void LibretroAudioAdapter::audio_output(ThreadState &thread, AudioOutPort &out_port, const void *buffer) {
    auto &port = static_cast<LibretroAudioOutPort &>(out_port);

    std::unique_lock<std::mutex> lock(port.mutex);
    // If ring buffer is full, wait for drain (retro_run will consume)
    if (port.nb_buffers_ready >= static_cast<int>(port.audio_buffers.size())) {
        thread.update_status(ThreadStatus::wait);
        port.cond_var.wait(lock, [&]() {
            return port.nb_buffers_ready < static_cast<int>(port.audio_buffers.size());
        });
        thread.update_status(ThreadStatus::run);
    }

    if (buffer) {
        memcpy(port.audio_buffers[port.next_write_buffer].data(), buffer, port.len_bytes);
        port.next_write_buffer = (port.next_write_buffer + 1) % port.audio_buffers.size();
        port.nb_buffers_ready++;
    }
}

void LibretroAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    // Volume is applied during drain
}

void LibretroAudioAdapter::switch_state(const bool pause) {
    // No-op for libretro — frontend controls pause
}

int LibretroAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    auto &port = static_cast<LibretroAudioOutPort &>(out_port);
    return port.nb_buffers_ready * port.len;
}

int libretro_audio_drain(AudioState &audio, std::vector<int16_t> &out_buffer) {
    out_buffer.clear();

    const std::lock_guard<std::mutex> lock(audio.mutex);

    for (auto it = audio.out_ports.begin(); it != audio.out_ports.end(); ++it) {
        LibretroAudioOutPort *port = static_cast<LibretroAudioOutPort *>(it->second.get());
        if (!port)
            continue;

        std::unique_lock<std::mutex> plock(port->mutex);
        while (port->nb_buffers_ready > 0) {
            const std::vector<uint8_t> &buf = port->audio_buffers[port->next_read_buffer];
            const int num_samples = port->len_bytes / static_cast<int>(sizeof(int16_t));
            const int16_t *samples = reinterpret_cast<const int16_t *>(buf.data());

            // Apply volume scaling
            const float vol_l = static_cast<float>(port->left_channel_volume) / static_cast<float>(SCE_AUDIO_OUT_MAX_VOL);
            const float vol_r = static_cast<float>(port->right_channel_volume) / static_cast<float>(SCE_AUDIO_OUT_MAX_VOL);
            const float vol = port->volume * audio.global_volume;

            if (port->channels == 2) {
                // Stereo — already in the format libretro expects
                for (int i = 0; i < num_samples; i += 2) {
                    int16_t l = static_cast<int16_t>(std::clamp(static_cast<int>(samples[i] * vol_l * vol), -32768, 32767));
                    int16_t r = static_cast<int16_t>(std::clamp(static_cast<int>(samples[i + 1] * vol_r * vol), -32768, 32767));
                    out_buffer.push_back(l);
                    out_buffer.push_back(r);
                }
            } else {
                // Mono — duplicate to stereo
                for (int i = 0; i < num_samples; i++) {
                    int16_t s = static_cast<int16_t>(std::clamp(static_cast<int>(samples[i] * vol_l * vol), -32768, 32767));
                    out_buffer.push_back(s);
                    out_buffer.push_back(s);
                }
            }

            port->next_read_buffer = (port->next_read_buffer + 1) % port->audio_buffers.size();
            port->nb_buffers_ready--;
        }
        plock.unlock();
        port->cond_var.notify_all();
    }

    // Return number of stereo frames
    return static_cast<int>(out_buffer.size() / 2);
}
