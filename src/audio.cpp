#include "medium/audio.hpp"
#include <numbers>
namespace med {
const char *bus_name(Bus bus) {
    switch (bus) {
    case Bus::Master:
        return "master";
    case Bus::Music:
        return "music";
    case Bus::Effects:
        return "effects";
    case Bus::Voice:
        return "voice";
    }
    return "master";
}

Mixer::Mixer(int sample_rate) : rate_(sample_rate) {
    if (sample_rate < 8000 || sample_rate > 192000)
        throw std::runtime_error("Unsupported audio sample rate");
    // Every buffer music will ever need, now: `render` and `stream_music` only move within these.
    for (auto &deck : decks_) {
        deck.ring.assign(music_ring_frames * 2, 0.0f);
        deck.scratch.assign(4096 * 2, 0);
    }
}

bool Mixer::play(const Sound &sound) {
    if (sound.sample) {
        const auto &sample = *sound.sample;
        if (sample.channels < 1 || sample.channels > 2 || sample.rate < 8000 || sample.rate > 192000 ||
            sample.frames() == 0 || sample.pcm.size() % static_cast<size_t>(sample.channels) != 0)
            throw std::runtime_error("A sample needs one or two channels, a rate of 8-192 kHz and a frame");
        if (!std::isfinite(sound.pitch) || sound.pitch < 0.25f || sound.pitch > 4)
            throw std::runtime_error("A sample's pitch is 0.25..4");
    } else {
        if (!std::isfinite(sound.frequency) || sound.frequency <= 0 || sound.frequency > 20000)
            throw std::runtime_error("Sound frequency is outside the audible range");
        if (!std::isfinite(sound.seconds) || sound.seconds <= 0 || sound.seconds > 60)
            throw std::runtime_error("Sound length is outside what a voice may hold");
    }
    if (!std::isfinite(sound.gain) || sound.gain < 0 || sound.gain > 1)
        throw std::runtime_error("Sound gain must be between zero and one");
    if (sound.positional && !finite(sound.position))
        throw std::runtime_error("Positional sound has no finite position");

    float gain = sound.gain, pan = 0;
    if (sound.positional) {
        const auto offset = sound.position - listener_.position;
        const float distance = length(offset);
        if (listener_.range <= 0 || distance >= listener_.range) {
            ++refused_; // inaudible; not an error, just not worth a voice
            return false;
        }
        // Linear falloff and linear pan. Both are simple on purpose: the shape can be tuned once
        // there is a real mix to tune it against, and a wrong curve is easier to hear than to
        // reason about.
        gain *= 1 - distance / listener_.range;
        pan = std::clamp(offset.x / listener_.range, -1.0f, 1.0f);
    }

    // A free voice, or the weakest one to evict.
    size_t chosen = max_voices;
    for (size_t i = 0; i < max_voices; ++i)
        if (!voices_[i].active) {
            chosen = i;
            break;
        }
    if (chosen == max_voices) {
        size_t weakest = 0;
        for (size_t i = 1; i < max_voices; ++i)
            if (voices_[i].priority < voices_[weakest].priority)
                weakest = i;
        // Strictly greater: equal priority keeps what is already playing, because cutting a sound
        // short to start an identical one is worse than dropping the new one.
        if (voices_[weakest].priority >= sound.priority) {
            ++refused_;
            return false;
        }
        chosen = weakest;
        ++evicted_;
    }
    auto &voice = voices_[chosen];
    voice.active = true;
    voice.frequency = sound.frequency;
    voice.gain = gain;
    voice.pan = pan;
    voice.bus = sound.bus;
    voice.priority = sound.priority;
    // At least one sample. A sound shorter than one truncated to zero, and the render's decrement
    // then wrapped it to the largest count there is: a silent voice holding its slot for ever.
    voice.total = voice.remaining =
        std::max<uint64_t>(1, static_cast<uint64_t>(sound.seconds * static_cast<float>(rate_)));
    voice.phase = 0;
    voice.sample = sound.sample;
    voice.position = 0;
    voice.step = sound.sample ? static_cast<double>(sound.sample->rate) / rate_ * sound.pitch : 0;
    ++started_;
    return true;
}
void Mixer::forget(const Sample *sample) {
    for (auto &voice : voices_)
        if (voice.sample == sample)
            voice.active = false;
}

// ---- Music ----
void Mixer::release(Deck &deck) {
    deck.stream.reset();
    deck.active = deck.primed = deck.ended = deck.drained = deck.retiring = false;
    deck.read = deck.write = deck.filled = deck.scratch_frames = deck.scratch_at = 0;
    deck.gain = deck.target = deck.step = 0;
    deck.position = 0;
}
void Mixer::play_music(std::unique_ptr<MusicStream> stream, float seconds, float gain, bool loop) {
    if (!stream || stream->channels() < 1 || stream->channels() > 2 || stream->rate() < 8000 ||
        stream->rate() > 192000)
        throw std::runtime_error("A music stream needs one or two channels and a rate of 8-192 kHz");
    if (!std::isfinite(gain) || gain < 0 || gain > 1)
        throw std::runtime_error("Music gain must be between zero and one");
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 60)
        throw std::runtime_error("A crossfade is 0..60 seconds");
    const size_t next = 1 - current_;
    // A track already fading out gives way at once: only two decks sound.
    release(decks_[next]);
    auto &outgoing = decks_[current_];
    if (outgoing.active) {
        if (seconds <= 0)
            release(outgoing);
        else {
            outgoing.retiring = true;
            outgoing.target = 0;
            outgoing.step = -outgoing.gain / (seconds * static_cast<float>(rate_));
            if (outgoing.step == 0)
                release(outgoing);
        }
    }
    auto &deck = decks_[next];
    deck.stream = std::move(stream);
    deck.active = true;
    deck.loop = loop;
    deck.target = gain;
    deck.gain = seconds > 0 ? 0 : gain;
    deck.step = seconds > 0 ? gain / (seconds * static_cast<float>(rate_)) : 0;
    current_ = next;
    fill(deck); // decoded ahead now, so the first mix has something to play
}
void Mixer::stop_music(float seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 60)
        throw std::runtime_error("A fade is 0..60 seconds");
    auto &deck = decks_[current_];
    if (!deck.active)
        return;
    if (seconds <= 0 || deck.gain <= 0) {
        release(deck);
        return;
    }
    deck.retiring = true;
    deck.target = 0;
    deck.step = -deck.gain / (seconds * static_cast<float>(rate_));
}
// The next source frame, as floats, or false at the end of a stream that does not loop.
bool Mixer::pull(Deck &deck, float (&frame)[2]) {
    const int channels = deck.stream->channels();
    if (deck.scratch_at == deck.scratch_frames) {
        deck.scratch_frames = deck.stream->read(deck.scratch);
        deck.scratch_at = 0;
        if (deck.scratch_frames == 0 && deck.loop) {
            deck.stream->rewind();
            deck.scratch_frames = deck.stream->read(deck.scratch);
        }
        if (deck.scratch_frames == 0)
            return false;
    }
    const auto *at = &deck.scratch[deck.scratch_at * static_cast<size_t>(channels)];
    frame[0] = static_cast<float>(at[0]) / 32768.0f;
    frame[1] = channels == 2 ? static_cast<float>(at[1]) / 32768.0f : frame[0];
    ++deck.scratch_at;
    return true;
}
void Mixer::fill(Deck &deck) {
    if (!deck.active || !deck.stream || deck.drained)
        return;
    // The last source frame is held as `next` when the stream ends, so it is played like every other
    // rather than lost between the two frames the output lies between.
    auto advance_source = [&] {
        deck.prev[0] = deck.next[0];
        deck.prev[1] = deck.next[1];
        if (!pull(deck, deck.next)) {
            deck.next[0] = deck.prev[0];
            deck.next[1] = deck.prev[1];
            deck.ended = true;
        }
    };
    if (!deck.primed) {
        if (!pull(deck, deck.next)) {
            deck.drained = true;
            return;
        }
        advance_source();
        deck.primed = true;
    }
    // Linear interpolation between source frames, so a 44.1 kHz track plays at pitch on a 48 kHz mix.
    const double advance = static_cast<double>(deck.stream->rate()) / rate_;
    while (deck.filled < music_ring_frames && !deck.drained) {
        const auto t = static_cast<float>(deck.position);
        deck.ring[deck.write * 2] = deck.prev[0] + (deck.next[0] - deck.prev[0]) * t;
        deck.ring[deck.write * 2 + 1] = deck.prev[1] + (deck.next[1] - deck.prev[1]) * t;
        deck.write = (deck.write + 1) % music_ring_frames;
        ++deck.filled;
        deck.position += advance;
        while (deck.position >= 1 && !deck.drained) {
            deck.position -= 1;
            if (deck.ended)
                deck.drained = true;
            else
                advance_source();
        }
    }
}
void Mixer::stream_music() {
    for (auto &deck : decks_) {
        if (!deck.active)
            continue;
        // Silent after its fade, or played out: released here, on the game thread, never in render.
        if ((deck.retiring && deck.gain <= 0 && deck.step == 0) || (deck.drained && deck.filled == 0)) {
            release(deck);
            continue;
        }
        fill(deck);
    }
}
const MusicStream *Mixer::music() const {
    const auto &deck = decks_[current_];
    return deck.active && !deck.retiring ? deck.stream.get() : nullptr;
}
size_t Mixer::music_decks() const {
    return static_cast<size_t>(decks_[0].active) + static_cast<size_t>(decks_[1].active);
}
float Mixer::music_gain() const {
    const auto &deck = decks_[current_];
    return deck.active ? deck.gain : 0;
}

void Mixer::set_gain(Bus bus, float gain) {
    if (!std::isfinite(gain) || gain < 0 || gain > 1)
        throw std::runtime_error("Bus gain must be between zero and one");
    const auto index = static_cast<size_t>(bus);
    gain_[index] = gain;
    fade_target_[index] = gain;
    fade_step_[index] = 0; // an immediate change cancels a fade rather than fighting it
}
float Mixer::gain(Bus bus) const { return gain_[static_cast<size_t>(bus)]; }

void Mixer::fade(Bus bus, float target, float seconds) {
    if (!std::isfinite(target) || target < 0 || target > 1)
        throw std::runtime_error("Bus gain must be between zero and one");
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 60)
        throw std::runtime_error("Fade length is outside what a bus may hold");
    const auto index = static_cast<size_t>(bus);
    fade_target_[index] = target;
    if (seconds <= 0) {
        gain_[index] = target;
        fade_step_[index] = 0;
        return;
    }
    const auto samples = static_cast<double>(seconds) * rate_;
    fade_step_[index] = static_cast<float>((target - gain_[index]) / samples);
}
bool Mixer::fading(Bus bus) const { return fade_step_[static_cast<size_t>(bus)] != 0; }

size_t Mixer::active() const {
    size_t count = 0;
    for (const auto &voice : voices_)
        count += voice.active ? 1 : 0;
    return count;
}
void Mixer::stop_all() {
    for (auto &voice : voices_)
        voice.active = false;
    for (auto &deck : decks_)
        release(deck);
}

void Mixer::render(std::span<float> interleaved) noexcept {
    // Everything below operates on members that already exist. No allocation, no exception, no
    // lock, no IO - this runs on the driver's thread against a deadline.
    const size_t frames = interleaved.size() / 2;
    const double step = 2.0 * std::numbers::pi_v<double> / static_cast<double>(rate_);
    for (size_t frame = 0; frame < frames; ++frame) {
        // Fades advance per sample, so a fade is smooth rather than stepping once a buffer.
        for (size_t bus = 0; bus < bus_count; ++bus) {
            if (fade_step_[bus] == 0)
                continue;
            gain_[bus] += fade_step_[bus];
            const bool arrived =
                fade_step_[bus] > 0 ? gain_[bus] >= fade_target_[bus] : gain_[bus] <= fade_target_[bus];
            if (arrived) {
                gain_[bus] = fade_target_[bus];
                fade_step_[bus] = 0;
            }
        }
        float left = 0, right = 0;
        // Music: each deck's decoded frame at its own fade, then the Music bus.
        float music_left = 0, music_right = 0;
        for (auto &deck : decks_) {
            if (!deck.active)
                continue;
            if (deck.step != 0) {
                deck.gain += deck.step;
                const bool arrived = deck.step > 0 ? deck.gain >= deck.target : deck.gain <= deck.target;
                if (arrived) {
                    deck.gain = deck.target;
                    deck.step = 0;
                }
            }
            if (deck.filled == 0) {
                if (!deck.drained)
                    ++underruns_;
                continue;
            }
            music_left += deck.ring[deck.read * 2] * deck.gain;
            music_right += deck.ring[deck.read * 2 + 1] * deck.gain;
            deck.read = (deck.read + 1) % music_ring_frames;
            --deck.filled;
        }
        const float music = gain_[static_cast<size_t>(Bus::Music)] * gain_[static_cast<size_t>(Bus::Master)];
        left += music_left * music;
        right += music_right * music;
        for (auto &voice : voices_) {
            if (!voice.active)
                continue;
            if (voice.sample) {
                // A sample voice: linear interpolation between its frames, at its rate and pitch.
                const auto &sample = *voice.sample;
                const auto length = sample.frames();
                const auto index = static_cast<size_t>(voice.position);
                if (index >= length) {
                    voice.active = false;
                    continue;
                }
                const auto t = static_cast<float>(voice.position - static_cast<double>(index));
                const auto channels = static_cast<size_t>(sample.channels);
                auto at = [&](size_t which, size_t channel) {
                    return static_cast<float>(
                               sample.pcm[which * channels + std::min(channel, channels - 1)]) /
                           32768.0f;
                };
                const float scale = voice.gain * gain_[static_cast<size_t>(voice.bus)] *
                                    gain_[static_cast<size_t>(Bus::Master)];
                // The final frame lasts one source-frame interval too. Hold it while resampling,
                // rather than dropping it (which also made a one-frame sample entirely silent).
                const auto next = std::min(index + 1, length - 1);
                const float l = at(index, 0) + (at(next, 0) - at(index, 0)) * t;
                const float r = at(index, 1) + (at(next, 1) - at(index, 1)) * t;
                left += l * scale * (1 - std::max(0.0f, voice.pan));
                right += r * scale * (1 + std::min(0.0f, voice.pan));
                voice.position += voice.step;
                if (voice.position >= static_cast<double>(length))
                    voice.active = false;
                continue;
            }
            // A linear decay to silence, so a voice never ends on a discontinuity - a tone cut off
            // mid-cycle is a click, and a room full of them is the loudest thing in the mix.
            const float envelope =
                voice.total ? static_cast<float>(voice.remaining) / static_cast<float>(voice.total) : 0;
            const auto sample = static_cast<float>(std::sin(voice.phase)) * voice.gain * envelope *
                                gain_[static_cast<size_t>(voice.bus)] *
                                gain_[static_cast<size_t>(Bus::Master)];
            // Equal-gain pan. Cheap, and adequate for tones; equal-power is worth having once
            // there is real material to hear the difference on.
            left += sample * (1 - std::max(0.0f, voice.pan));
            right += sample * (1 + std::min(0.0f, voice.pan));
            voice.phase += step * voice.frequency;
            if (voice.phase > 2.0 * std::numbers::pi_v<double>)
                voice.phase -= 2.0 * std::numbers::pi_v<double>;
            if (--voice.remaining == 0)
                voice.active = false;
        }
        // Limited rather than allowed to wrap. Counted, because a non-zero count is a budget
        // problem worth knowing about rather than a fault to hide.
        if (left > 1 || left < -1 || right > 1 || right < -1)
            ++clipped_;
        interleaved[frame * 2] = std::clamp(left, -1.0f, 1.0f);
        interleaved[frame * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
}
} // namespace med
