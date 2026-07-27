#include "audio_player.h"

#include <iostream>

#include <SDL2/SDL.h>

namespace pm::stream {

namespace {
// Ugg! If the PC hiccups, sound piles up in the queue and then plays late forever.
// Cave man throws away the backlog once it grows past this, so picture and sound
// walk together again. A quarter heartbeat of sound is plenty of cushion.
constexpr uint32_t MAX_QUEUE_MS = 250;
}

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::start(int sample_rate, int channels) {
    stop();

    if (sample_rate <= 0 || channels <= 0) {
        return false;
    }

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::cerr << "[Audio] No sound subsystem: " << SDL_GetError() << std::endl;
            return false;
        }
        m_owns_subsystem = true;
    }

    SDL_AudioSpec want = {};
    want.freq = sample_rate;
    want.format = AUDIO_S16LSB; // scrcpy raw audio is signed 16-bit little endian
    want.channels = static_cast<Uint8>(channels);
    want.samples = 1024;
    want.callback = nullptr; // cave man pushes sound himself via SDL_QueueAudio

    SDL_AudioSpec have = {};
    const SDL_AudioDeviceID device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device == 0) {
        std::cerr << "[Audio] Could not open sound device: " << SDL_GetError() << std::endl;
        if (m_owns_subsystem) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            m_owns_subsystem = false;
        }
        return false;
    }

    m_bytes_per_second = static_cast<uint32_t>(have.freq) * have.channels * sizeof(int16_t);
    m_device.store(device);
    m_running.store(true);
    SDL_PauseAudioDevice(device, 0); // 0 = play

    std::cout << "[Audio] Sound device open: " << have.freq << " Hz, "
              << static_cast<int>(have.channels) << " channels" << std::endl;
    return true;
}

void AudioPlayer::feed(const uint8_t* data, size_t size) {
    const SDL_AudioDeviceID device = m_device.load();
    if (!m_running.load() || device == 0 || !data || size == 0) {
        return;
    }

    if (m_bytes_per_second > 0) {
        const uint32_t queued = SDL_GetQueuedAudioSize(device);
        const uint32_t limit = m_bytes_per_second * MAX_QUEUE_MS / 1000;
        if (queued > limit) {
            SDL_ClearQueuedAudio(device);
        }
    }

    SDL_QueueAudio(device, data, static_cast<Uint32>(size));
}

void AudioPlayer::stop() {
    const SDL_AudioDeviceID device = m_device.exchange(0);
    m_running.store(false);

    if (device != 0) {
        SDL_PauseAudioDevice(device, 1);
        SDL_ClearQueuedAudio(device);
        SDL_CloseAudioDevice(device);
    }

    if (m_owns_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_owns_subsystem = false;
    }
    m_bytes_per_second = 0;
}

} // namespace pm::stream
