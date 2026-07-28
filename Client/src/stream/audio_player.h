#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace pm::stream {

// Cave man carry phone sound to PC speaker.
//
// Ugg! No decoder here on purpose. The scrcpy server can hand us plain PCM
// (audio_codec=raw), so the sound needs no unpacking at all — no codec config
// dance, no extra FFmpeg library, and nothing that can desync. It costs about
// 1.5 Mbit/s next to a 20 Mbit/s video stream, which on a home WLAN is nothing.
class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Opens the default output device for signed 16-bit PCM.
    // Returns false if there is no sound device — the stream must keep going anyway.
    bool start(int sample_rate, int channels);

    // Hands freshly arrived PCM to the speaker queue.
    void feed(const uint8_t* data, size_t size);

    void stop();
    bool is_running() const { return m_running.load(); }

private:
    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_device{0}; // SDL_AudioDeviceID
    bool m_owns_subsystem{false};
    uint32_t m_bytes_per_second{0};
};

} // namespace pm::stream
