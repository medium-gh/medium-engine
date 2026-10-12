#include "medium/audio_file.hpp"
#include <cstdlib>
#include <cstring>
// The pinned stb_vorbis, compiled here and nowhere else. Its own diagnostics are the library's: the
// directory is a system include, as stb_image's is for the renderer. GCC's optimiser still reports
// one maybe-uninitialized read inside it in the release build (a false positive in its header parser,
// recorded by the tile-terrain verify), so that one diagnostic is off for the library's lines alone.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <stb_vorbis.c>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
namespace med {
namespace {
uint32_t le32(std::span<const uint8_t> b, size_t at) {
    return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8 |
           static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
}
uint16_t le16(std::span<const uint8_t> b, size_t at) { return static_cast<uint16_t>(b[at] | b[at + 1] << 8); }
void check_shape(int channels, int rate, size_t frames, const char *format) {
    if (channels < 1 || channels > 2)
        throw std::runtime_error(std::string(format) + ": " + std::to_string(channels) +
                                 " channels; a sound has one or two");
    if (rate < 8000 || rate > 192000)
        throw std::runtime_error(std::string(format) + ": a rate of " + std::to_string(rate) +
                                 " Hz; a sound is 8-192 kHz");
    if (frames == 0)
        throw std::runtime_error(std::string(format) + ": no audio in it");
    if (frames > max_sound_frames)
        throw std::runtime_error(std::string(format) + ": longer than a minute at 192 kHz");
}
Sample decode_wav(std::span<const uint8_t> b) {
    if (b.size() < 12)
        throw std::runtime_error("WAV: too short to be one");
    int format = 0, channels = 0, rate = 0, bits = 0;
    std::span<const uint8_t> data;
    bool have_format = false, have_data = false;
    // Chunks after the RIFF header, each padded to an even length.
    for (size_t at = 12; at + 8 <= b.size();) {
        const auto size = le32(b, at + 4);
        if (size > b.size() - at - 8)
            throw std::runtime_error("WAV: a chunk runs past the end of the file");
        const auto body = b.subspan(at + 8, size);
        if (std::memcmp(&b[at], "fmt ", 4) == 0) {
            if (size < 16)
                throw std::runtime_error("WAV: a format chunk too short to describe anything");
            format = le16(body, 0);
            channels = le16(body, 2);
            rate = static_cast<int>(le32(body, 4));
            bits = le16(body, 14);
            // WAVE_FORMAT_EXTENSIBLE carries the real format in its sub-format GUID's first two bytes.
            if (format == 0xFFFE && size >= 26)
                format = le16(body, 24);
            have_format = true;
        } else if (std::memcmp(&b[at], "data", 4) == 0) {
            data = body;
            have_data = true;
        }
        at += 8 + size + (size & 1);
    }
    if (!have_format || !have_data)
        throw std::runtime_error("WAV: no format or no data chunk");
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24);
    const bool floating = format == 3 && bits == 32;
    if (!pcm && !floating)
        throw std::runtime_error("WAV: format " + std::to_string(format) + " at " + std::to_string(bits) +
                                 " bits; only PCM of 8, 16 or 24 bits and 32-bit float are read");
    const size_t width = static_cast<size_t>(bits / 8);
    if (channels < 1)
        throw std::runtime_error("WAV: no channels");
    const size_t frames = data.size() / (width * static_cast<size_t>(channels));
    check_shape(channels, rate, frames, "WAV");
    Sample sample;
    sample.rate = rate;
    sample.channels = channels;
    sample.pcm.resize(frames * static_cast<size_t>(channels));
    for (size_t i = 0; i < sample.pcm.size(); ++i) {
        const size_t at = i * width;
        int32_t value = 0;
        if (floating) {
            float f;
            const uint32_t bits32 = le32(data, at);
            std::memcpy(&f, &bits32, sizeof f);
            value = static_cast<int32_t>(
                std::lround(std::clamp(std::isfinite(f) ? f : 0.0f, -1.0f, 1.0f) * 32767.0f));
        } else if (width == 1)
            value = (static_cast<int32_t>(data[at]) - 128) << 8;
        else if (width == 2)
            value = static_cast<int16_t>(le16(data, at));
        else
            value = static_cast<int32_t>(static_cast<uint32_t>(data[at + 1]) << 16 |
                                         static_cast<uint32_t>(data[at + 2]) << 24) >>
                    16;
        sample.pcm[i] = static_cast<int16_t>(value);
    }
    return sample;
}
// stb_vorbis over bytes this object owns, decoded as it is read.
class VorbisStream final : public MusicStream {
    std::vector<uint8_t> bytes_;
    stb_vorbis *vorbis_ = nullptr;
    int rate_ = 0, channels_ = 0;

  public:
    explicit VorbisStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
        int error = 0;
        vorbis_ = stb_vorbis_open_memory(bytes_.data(), static_cast<int>(bytes_.size()), &error, nullptr);
        if (!vorbis_)
            throw std::runtime_error("Ogg Vorbis: not a stream stb_vorbis can open (error " +
                                     std::to_string(error) + ")");
        const auto info = stb_vorbis_get_info(vorbis_);
        rate_ = static_cast<int>(info.sample_rate);
        channels_ = info.channels;
        try {
            check_shape(channels_, rate_, 1, "Ogg Vorbis");
        } catch (...) {
            stb_vorbis_close(vorbis_);
            throw;
        }
    }
    ~VorbisStream() override { stb_vorbis_close(vorbis_); }
    VorbisStream(const VorbisStream &) = delete;
    VorbisStream &operator=(const VorbisStream &) = delete;
    int rate() const override { return rate_; }
    int channels() const override { return channels_; }
    size_t read(std::span<int16_t> out) override {
        const int got = stb_vorbis_get_samples_short_interleaved(vorbis_, channels_, out.data(),
                                                                 static_cast<int>(out.size()));
        return got > 0 ? static_cast<size_t>(got) : 0;
    }
    void rewind() override { stb_vorbis_seek_start(vorbis_); }
};
} // namespace

std::string_view audio_format(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(&bytes[8], "WAVE", 4) == 0)
        return "wav";
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0)
        return "ogg";
    return {};
}
Sample decode_sound(std::span<const uint8_t> bytes) {
    if (bytes.size() > max_audio_file_bytes)
        throw std::runtime_error("A sound file may be at most 64 MB");
    const auto format = audio_format(bytes);
    if (format == "wav")
        return decode_wav(bytes);
    if (format != "ogg")
        throw std::runtime_error("Not a WAV or an Ogg Vorbis file");
    int channels = 0, rate = 0;
    short *output = nullptr;
    const int frames =
        stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &output);
    if (frames < 0 || !output) {
        std::free(output);
        throw std::runtime_error("Ogg Vorbis: the stream would not decode");
    }
    Sample sample;
    try {
        check_shape(channels, rate, static_cast<size_t>(frames), "Ogg Vorbis");
        sample.rate = rate;
        sample.channels = channels;
        sample.pcm.assign(output, output + static_cast<size_t>(frames) * static_cast<size_t>(channels));
    } catch (...) {
        std::free(output);
        throw;
    }
    std::free(output);
    return sample;
}
std::unique_ptr<MusicStream> open_music(std::vector<uint8_t> bytes) {
    if (bytes.size() > max_audio_file_bytes)
        throw std::runtime_error("A music file may be at most 64 MB");
    const auto format = audio_format(bytes);
    if (format == "wav")
        return std::make_unique<SampleStream>(std::make_shared<const Sample>(decode_wav(bytes)));
    if (format == "ogg")
        return std::make_unique<VorbisStream>(std::move(bytes));
    throw std::runtime_error("Not a WAV or an Ogg Vorbis file");
}
SampleStream::SampleStream(std::shared_ptr<const Sample> sample) : sample_(std::move(sample)) {
    if (!sample_ || sample_->frames() == 0)
        throw std::runtime_error("A sample stream needs a sample with audio in it");
}
size_t SampleStream::read(std::span<int16_t> out) {
    const auto channels = static_cast<size_t>(sample_->channels);
    const size_t frames = std::min(out.size() / channels, sample_->frames() - at_);
    std::copy_n(sample_->pcm.begin() + static_cast<std::ptrdiff_t>(at_ * channels), frames * channels,
                out.begin());
    at_ += frames;
    return frames;
}
} // namespace med
