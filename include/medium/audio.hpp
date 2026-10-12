#pragma once
#include "medium/core.hpp"
#include <memory>
#include <vector>
namespace med {
// A small mixer: buses, bounded voices, fades and positional placement.
//
// The audio callback is the one place in this project where the usual tools are unavailable.
// It runs on a thread owned by the driver, on a deadline measured in milliseconds, and anything
// that blocks it produces an audible fault rather than a slow frame. So `render` below **allocates
// nothing, throws nothing, locks nothing and touches no file**. Every voice lives in a fixed array
// that exists before the callback does.
//
// That is a claim worth testing rather than asserting, and the audio suite tests it by counting
// global allocations across a render.
//
// What this is not: a sound engine. Voices are generated tones, because that is what the project
// has - `Renderer::tone` synthesised a decaying sine and queued it, and there is no sample loading
// yet. The structure is what was missing: something playing a sound had no way to say which bus it
// belonged on, no limit on how many could play at once, and no way to be quieter for being far
// away.

enum class Bus : uint8_t { Master, Music, Effects, Voice };
inline constexpr size_t bus_count = 4;
const char *bus_name(Bus bus);

// How many voices may sound at once. A cap rather than a hope: without one, a room full of
// fighters lands every hit sound on the same tick and clips.
inline constexpr size_t max_voices = 32;

// A decoded sound: interleaved 16-bit PCM at its own rate, one or two channels (`audio_file.hpp`
// reads one from a WAV or an Ogg Vorbis file). Immutable once loaded. A voice plays it by pointer, so
// whatever holds it must outlive every voice playing it - `Mixer::forget` stops those first.
struct Sample {
    int rate = 48000;
    int channels = 1;
    std::vector<int16_t> pcm;
    size_t frames() const { return channels > 0 ? pcm.size() / static_cast<size_t>(channels) : 0; }
};
// Where a music deck's PCM comes from: a track decoded as it plays, so a three-minute track is its
// compressed bytes and a small window rather than thirty megabytes of samples. Read only on the game
// thread (`Mixer::stream_music`), never by `render`.
class MusicStream {
  public:
    virtual ~MusicStream() = default;
    virtual int rate() const = 0;
    virtual int channels() const = 0;
    // Up to `out.size() / channels()` frames of interleaved PCM; fewer, then none, at the end.
    virtual size_t read(std::span<int16_t> out) = 0;
    virtual void rewind() = 0;
};

struct Listener {
    Vec2 position{};
    // Beyond this a sound is inaudible. Also the distance over which panning reaches full.
    float range = 900;
};

struct Sound {
    float frequency = 440;
    float gain = 1;
    float seconds = 0.1f;
    Bus bus = Bus::Effects;
    // Which voice survives when the mixer is full. Higher wins; equal priority keeps what is
    // already playing, because cutting a sound short to start an identical one is worse than
    // dropping the new one.
    int priority = 0;
    Vec2 position{};
    bool positional = false;
    // Play this decoded sound instead of a tone: `frequency` and `seconds` are then unused, and the
    // voice lasts as long as the sample at `pitch` (0.25..4, a playback-rate multiplier).
    const Sample *sample = nullptr;
    float pitch = 1;
};
// How much music a deck holds decoded ahead of the mix: about a third of a second at 48 kHz, filled
// once a frame by `stream_music`. A frame longer than that starves the deck (counted, silent).
inline constexpr size_t music_ring_frames = 16384;

class Mixer {
    struct Playing {
        bool active = false;
        float frequency = 0, gain = 0, pan = 0;
        Bus bus = Bus::Effects;
        int priority = 0;
        uint64_t remaining = 0, total = 0;
        double phase = 0;
        const Sample *sample = nullptr; // a sample voice when set; a tone otherwise
        double position = 0, step = 0;  // frames into the sample, and frames per output frame
    };
    // A music deck: a stream decoded ahead into a fixed ring, at the mixer's rate, and its own fade.
    // Two decks make a crossfade: the outgoing track fades out on one while the new one fades in on
    // the other.
    struct Deck {
        std::unique_ptr<MusicStream> stream;
        std::vector<float> ring;      // stereo frames at the mixer's rate; sized once
        std::vector<int16_t> scratch; // source frames read from the stream; sized once
        size_t read = 0, write = 0, filled = 0, scratch_frames = 0, scratch_at = 0;
        float gain = 0, target = 0, step = 0;
        float prev[2]{}, next[2]{}; // the two source frames the output lies between
        double position = 0;        // how far between them, 0..1
        bool active = false, loop = true, primed = false;
        bool ended = false;    // the stream has no more frames: `next` holds its last
        bool drained = false;  // ... and that last frame has been decoded into the ring
        bool retiring = false; // fading out; released by `stream_music` once silent
    };
    std::array<Playing, max_voices> voices_{};
    std::array<Deck, 2> decks_{};
    size_t current_ = 0; // the deck holding the current track; the other is outgoing or idle
    uint64_t underruns_ = 0;
    bool pull(Deck &deck, float (&frame)[2]);
    void fill(Deck &deck);
    void release(Deck &deck);
    std::array<float, bus_count> gain_{1, 1, 1, 1};
    std::array<float, bus_count> fade_target_{1, 1, 1, 1};
    std::array<float, bus_count> fade_step_{};
    Listener listener_{};
    int rate_;
    size_t started_ = 0, refused_ = 0, evicted_ = 0;
    uint64_t clipped_ = 0;

  public:
    explicit Mixer(int sample_rate = 48000);
    int sample_rate() const { return rate_; }

    // Throws on a sound this mixer cannot represent. Called from gameplay, never the callback.
    bool play(const Sound &sound);
    void set_listener(Listener listener) { listener_ = listener; }
    Listener listener() const { return listener_; }
    // Immediate. Throws outside 0..1, because a gain above one is a clip waiting to happen and a
    // negative one inverts the waveform.
    void set_gain(Bus bus, float gain);
    float gain(Bus bus) const;
    // Over `seconds`, from wherever the bus is now. Zero seconds is immediate. A fade in progress
    // is replaced rather than queued: the newest instruction is the one the player asked for.
    void fade(Bus bus, float target, float seconds);
    bool fading(Bus bus) const;

    size_t active() const;
    size_t started() const { return started_; }
    size_t refused() const { return refused_; }
    size_t evicted() const { return evicted_; }
    // Samples that would have exceeded full scale and were limited. Non-zero means the mix is too
    // loud for its bus gains, which is a budget problem rather than a bug.
    uint64_t clipped() const { return clipped_; }
    void stop_all();
    // Stops every voice playing `sample`, so its owner may free it.
    void forget(const Sample *sample);

    // ---- Music: the Music bus's two decks ----
    // Starts `stream` as the current track, faded in over `seconds` to `gain` while whatever was
    // playing fades out over the same time (zero cuts). A third track mid-crossfade drops the one
    // already fading out. Throws, changing nothing, on a stream this mixer cannot play. Game thread.
    void play_music(std::unique_ptr<MusicStream> stream, float seconds, float gain = 1, bool loop = true);
    // Fades the current track out over `seconds` and stops it.
    void stop_music(float seconds);
    // Decodes ahead into each deck's ring and releases decks that finished. Once a frame, on the
    // game thread, before `render`: this is where music is decoded, never in `render`.
    void stream_music();
    // The current track's stream, or null when none plays or it is fading out.
    const MusicStream *music() const;
    // Decks sounding, fading ones included: 2 during a crossfade.
    size_t music_decks() const;
    // The current track's gain now, 0..1.
    float music_gain() const;
    // Frames the mix wanted from a deck that had none decoded: a frame that ran long.
    uint64_t music_underruns() const { return underruns_; }

    // Fills interleaved stereo. **This is what an audio callback calls**: no allocation, no
    // exception, no lock, no IO, no decoding - music comes from what `stream_music` decoded ahead.
    // Advances fades and retires finished voices as it goes. `render` and the calls above are made
    // from one thread (the renderer pumps audio on the game thread), so nothing here is shared.
    void render(std::span<float> interleaved) noexcept;
};
} // namespace med
