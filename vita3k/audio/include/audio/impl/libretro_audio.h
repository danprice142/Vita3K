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

#include "../state.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

// Libretro audio adapter: captures PCM samples into a ring buffer
// that is drained each frame by retro_run() and sent to the frontend
// via retro_audio_sample_batch_t.
//
// The emulator's audio threads call audio_output() which writes S16LE
// interleaved stereo samples into the ring buffer. The libretro frontend
// expects the same format: interleaved int16_t (L,R,L,R,...).

struct LibretroAudioOutPort : AudioOutPort {
    std::mutex mutex;
    std::condition_variable cond_var;
    // Ring buffer of audio data chunks
    std::vector<std::vector<uint8_t>> audio_buffers;
    int next_write_buffer = 0;
    int next_read_buffer = 0;
    std::atomic<int> nb_buffers_ready{ 0 };

    int channels = 2;
};

class LibretroAudioAdapter : public AudioAdapter {
public:
    LibretroAudioAdapter(AudioState &audio_state);
    ~LibretroAudioAdapter() override = default;

    bool init() override;
    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override;
    void audio_output(ThreadState &thread, AudioOutPort &out_port, const void *buffer) override;
    void set_volume(AudioOutPort &out_port, float volume) override;
    void switch_state(const bool pause) override;
    int get_rest_sample(AudioOutPort &out_port) override;
};

// Drain all available audio from all ports into a single interleaved stereo
// int16_t buffer suitable for retro_audio_sample_batch_t.
// Returns the number of stereo frames (each frame = 2 int16_t samples).
int libretro_audio_drain(AudioState &audio, std::vector<int16_t> &out_buffer);
