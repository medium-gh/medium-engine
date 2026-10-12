#pragma once
#include "medium/audio.hpp"
#include <memory>
#include <span>
namespace med {
// Sound files: what the mixer plays when it is given something other than a tone.
//
// Two formats, told apart by their first bytes rather than by a file's name: RIFF WAVE (PCM of 8, 16
// or 24 bits, or 32-bit float) and Ogg Vorbis, through the pinned stb_vorbis. One or two channels,
// 8-192 kHz. Anything else is refused with what it was, because a sound that silently fails to load
// sounds exactly like a sound that was never hooked up.
//
// A *sound* is decoded whole, once, when it loads: it is short and is started many times. A *track* is
// decoded as it plays (`open_music`), from its compressed bytes held in memory.

// Refuses anything longer than this: a sound is a blow or a footstep, a track is music.
inline constexpr size_t max_sound_frames = 60 * 192000;
inline constexpr size_t max_audio_file_bytes = 64 * 1024 * 1024;

// "wav", "ogg", or empty when the bytes are neither.
std::string_view audio_format(std::span<const uint8_t> bytes);
// Decodes a whole sound. Throws, naming what was wrong, on anything it cannot play.
Sample decode_sound(std::span<const uint8_t> bytes);
// A track to play on a music deck, decoded as it plays. Takes the file's bytes; throws on anything it
// cannot play. A WAV track is decoded whole (it is already PCM).
std::unique_ptr<MusicStream> open_music(std::vector<uint8_t> bytes);
// A decoded sample played as a track (the WAV case, and tests).
class SampleStream final : public MusicStream {
    std::shared_ptr<const Sample> sample_;
    size_t at_ = 0;

  public:
    explicit SampleStream(std::shared_ptr<const Sample> sample);
    int rate() const override { return sample_->rate; }
    int channels() const override { return sample_->channels; }
    size_t read(std::span<int16_t> out) override;
    void rewind() override { at_ = 0; }
};
} // namespace med
