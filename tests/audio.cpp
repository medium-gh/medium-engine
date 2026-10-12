#include "medium/audio.hpp"
#include "medium/audio_file.hpp"
#include <cstring>
#include <iostream>
using namespace med;

// Global allocation counting. The claim "the audio callback allocates nothing" is the one thing in
// this file that cannot be checked by looking at the code and staying convinced a year later, so
// it is measured: every `new` in the process goes through here, and the render is bracketed.
namespace {
std::size_t allocations = 0;
bool counting = false;
} // namespace
// These *replace* the global operators, so every pairing below is malloc/free and correct. GCC's
// -Wmismatched-new-delete cannot see through a replacement at -O2: it reasons about the `new`
// expressions in this file, decides their storage came from the real operator new, and then objects
// to `free` receiving it. Narrowly suppressed rather than worked around, because working around it
// would mean not counting allocations, which is the entire point of this file.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void *operator new(std::size_t size) {
    if (counting)
        ++allocations;
    if (void *memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void *memory) noexcept { operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { operator delete(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

int main(int argc, char **argv) {
    int checks = 0;
    try {
        if (argc != 2)
            throw std::runtime_error("Pass the test data directory");
        const fs::path data = argv[1];
        auto check = [&](bool pass, const char *message) {
            ++checks;
            if (!pass)
                throw std::runtime_error(message);
        };
        auto rejects = [&](auto action, const char *message) {
            bool threw = false;
            try {
                action();
            } catch (const std::runtime_error &) {
                threw = true;
            }
            check(threw, message);
        };
        auto loudest = [](std::span<const float> buffer) {
            float peak = 0;
            for (float sample : buffer)
                peak = std::max(peak, std::abs(sample));
            return peak;
        };

        // ---- What a sound must be ----
        {
            Mixer mixer;
            check(mixer.active() == 0 && mixer.started() == 0, "A new mixer is silent");
            rejects([&] { mixer.play({0}); }, "A sound with no frequency is refused");
            rejects([&] { mixer.play({30000}); }, "And one above hearing");
            rejects([&] { mixer.play({440, 2}); }, "A gain above full scale is refused, not clamped");
            rejects([&] { mixer.play({440, -1}); }, "And a negative one, which inverts the waveform");
            rejects([&] { mixer.play({440, 1, 0}); }, "A sound of no length is refused");
            rejects([&] { mixer.play({440, 1, 1000}); }, "And one longer than a voice may hold");
            {
                // Shorter than one sample: it plays for one and ends, rather than truncating to zero
                // and wrapping to a voice that never ends.
                Mixer brief;
                check(brief.play({440, 1, 1e-6f}), "A sound shorter than one sample is accepted");
                std::array<float, 64> buffer{};
                brief.render(buffer);
                check(brief.active() == 0, "And it ends, freeing its voice");
            }
            rejects([&] { Mixer bad(10); }, "An unsupported sample rate is refused");
            rejects([&] { mixer.set_gain(Bus::Music, 4); }, "A bus gain above full scale is refused");
            rejects([&] { mixer.fade(Bus::Music, 0.5f, -1); }, "And a fade of negative length");
        }

        // ---- The voice limit, and which voice loses ----
        {
            Mixer mixer;
            for (size_t i = 0; i < max_voices; ++i)
                check(mixer.play({440, 1, 1, Bus::Effects, 5}), "Voices start while there is room");
            check(mixer.active() == max_voices, "Up to the limit and no further");
            check(!mixer.play({440, 1, 1, Bus::Effects, 5}),
                  "An equal-priority sound is refused when full: cutting one short to start an "
                  "identical one is worse than dropping the new one");
            check(mixer.refused() == 1 && mixer.evicted() == 0, "Refused rather than evicted");
            check(mixer.play({440, 1, 1, Bus::Effects, 9}),
                  "A more important sound displaces the weakest voice");
            check(mixer.evicted() == 1 && mixer.active() == max_voices,
                  "Still at the limit: one out, one in");
            check(!mixer.play({440, 1, 1, Bus::Effects, 4}), "And a less important one is still refused");
            mixer.stop_all();
            check(mixer.active() == 0, "Everything can be stopped");
        }

        // ---- Buses ----
        {
            Mixer mixer(48000);
            std::vector<float> buffer(960 * 2);
            mixer.play({440, 1, 1, Bus::Music});
            mixer.render(buffer);
            const float full = loudest(buffer);
            check(full > 0.1f, "A playing voice makes sound");

            mixer.stop_all();
            mixer.set_gain(Bus::Music, 0.25f);
            mixer.play({440, 1, 1, Bus::Music});
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mixer.render(buffer);
            const float quiet = loudest(buffer);
            check(quiet < full * 0.4f, "Its bus gain makes it quieter");

            mixer.stop_all();
            mixer.set_gain(Bus::Music, 1);
            mixer.set_gain(Bus::Master, 0);
            mixer.play({440, 1, 1, Bus::Music});
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mixer.render(buffer);
            check(loudest(buffer) == 0,
                  "And master silences everything, whatever its own bus says - which is what makes "
                  "it a master");

            mixer.set_gain(Bus::Master, 1);
            mixer.stop_all();
            mixer.set_gain(Bus::Effects, 0);
            mixer.play({440, 1, 1, Bus::Music});
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mixer.render(buffer);
            check(loudest(buffer) > 0.1f, "Silencing one bus does not silence another");
        }

        // ---- Fades ----
        {
            Mixer mixer(48000);
            std::vector<float> buffer(4800 * 2); // a tenth of a second
            mixer.set_gain(Bus::Music, 1);
            mixer.fade(Bus::Music, 0, 1.0f);
            check(mixer.fading(Bus::Music), "A fade is in progress");
            mixer.render(buffer);
            check(mixer.gain(Bus::Music) < 1 && mixer.gain(Bus::Music) > 0.8f,
                  "A tenth of the way through a one-second fade, it is a tenth of the way down - "
                  "advanced per sample, so a fade is smooth rather than stepping once a buffer");
            check(mixer.fading(Bus::Music), "And still going");
            for (int i = 0; i < 10; ++i)
                mixer.render(buffer);
            check(mixer.gain(Bus::Music) == 0 && !mixer.fading(Bus::Music), "It arrives exactly, and stops");

            mixer.fade(Bus::Music, 1, 0);
            check(mixer.gain(Bus::Music) == 1 && !mixer.fading(Bus::Music),
                  "A fade of no length is immediate");
            mixer.fade(Bus::Music, 0, 5);
            mixer.set_gain(Bus::Music, 0.5f);
            check(!mixer.fading(Bus::Music) && mixer.gain(Bus::Music) == 0.5f,
                  "And setting a gain outright cancels a fade rather than fighting it");
        }

        // ---- Positional ----
        {
            Mixer mixer(48000);
            mixer.set_listener({{0, 0}, 900});
            std::vector<float> buffer(960 * 2);

            mixer.play({440, 1, 1, Bus::Effects, 0, {800, 0}, true});
            mixer.render(buffer);
            float far_left = 0, far_right = 0;
            for (size_t i = 0; i < buffer.size(); i += 2) {
                far_left = std::max(far_left, std::abs(buffer[i]));
                far_right = std::max(far_right, std::abs(buffer[i + 1]));
            }
            check(far_right > far_left,
                  "A sound to the right is louder on the right - which is the entire point of "
                  "knowing where it is");
            const float distant = std::max(far_left, far_right);

            mixer.stop_all();
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mixer.play({440, 1, 1, Bus::Effects, 0, {40, 0}, true});
            mixer.render(buffer);
            check(loudest(buffer) > distant, "And a nearer one is louder than a distant one");

            mixer.stop_all();
            check(!mixer.play({440, 1, 1, Bus::Effects, 0, {5000, 0}, true}),
                  "Something out of earshot does not take a voice: being inaudible is not an error, "
                  "it is simply not worth one of thirty-two slots");
            rejects([&] { mixer.play({440, 1, 1, Bus::Effects, 0, {NAN, 0}, true}); },
                    "But a position that is not a position is an error");

            mixer.stop_all();
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mixer.play({440, 1, 1, Bus::Effects, 0, {0, 0}, true});
            mixer.render(buffer);
            float centre_left = 0, centre_right = 0;
            for (size_t i = 0; i < buffer.size(); i += 2) {
                centre_left = std::max(centre_left, std::abs(buffer[i]));
                centre_right = std::max(centre_right, std::abs(buffer[i + 1]));
            }
            check(std::abs(centre_left - centre_right) < .001f, "A sound at the listener is centred");
        }

        // ---- The mix stays inside full scale ----
        {
            Mixer mixer(48000);
            for (size_t i = 0; i < max_voices; ++i)
                mixer.play({440, 1, 1, Bus::Effects, 0});
            std::vector<float> buffer(4800 * 2);
            mixer.render(buffer);
            check(loudest(buffer) <= 1.0f,
                  "Thirty-two voices at once are limited rather than allowed to wrap, which is the "
                  "difference between loud and a crackle");
            check(mixer.clipped() > 0,
                  "And the limiting is counted: a non-zero count is a budget problem worth knowing "
                  "about, not a fault to hide");
        }

        // ---- Sound files ----
        // A WAV written here, byte by byte, so every field the reader trusts is one a test chose.
        auto wav = [](int format, int channels, int rate, int bits, const std::vector<uint8_t> &body) {
            std::vector<uint8_t> out;
            auto u32 = [&](uint32_t v) {
                for (int i = 0; i < 4; ++i)
                    out.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            auto u16 = [&](uint16_t v) {
                out.push_back(static_cast<uint8_t>(v));
                out.push_back(static_cast<uint8_t>(v >> 8));
            };
            auto tag = [&](const char *t) { out.insert(out.end(), t, t + 4); };
            tag("RIFF");
            u32(static_cast<uint32_t>(36 + body.size()));
            tag("WAVE");
            tag("fmt ");
            u32(16);
            u16(static_cast<uint16_t>(format));
            u16(static_cast<uint16_t>(channels));
            u32(static_cast<uint32_t>(rate));
            u32(static_cast<uint32_t>(rate * channels * bits / 8));
            u16(static_cast<uint16_t>(channels * bits / 8));
            u16(static_cast<uint16_t>(bits));
            tag("data");
            u32(static_cast<uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
            return out;
        };
        auto pcm16 = [](std::initializer_list<int16_t> values) {
            std::vector<uint8_t> out;
            for (const auto v : values) {
                out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v)));
                out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8));
            }
            return out;
        };
        {
            const auto mono = decode_sound(wav(1, 1, 22050, 16, pcm16({0, 1000, -1000, 32767, -32768})));
            check(mono.channels == 1 && mono.rate == 22050 && mono.frames() == 5 &&
                      mono.pcm == std::vector<int16_t>{0, 1000, -1000, 32767, -32768},
                  "A 16-bit WAV decodes to its own samples");
            const auto stereo = decode_sound(wav(1, 2, 48000, 16, pcm16({1, 2, 3, 4})));
            check(stereo.channels == 2 && stereo.frames() == 2 && stereo.pcm[3] == 4,
                  "Stereo stays interleaved");
            const auto eight = decode_sound(wav(1, 1, 8000, 8, {128, 255, 0}));
            check(eight.pcm == std::vector<int16_t>{0, 127 << 8, -128 << 8},
                  "8-bit PCM is unsigned about 128");
            const auto deep = decode_sound(wav(1, 1, 8000, 24, {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0}));
            check(deep.pcm == std::vector<int16_t>{16384, -16384}, "24-bit PCM keeps its top sixteen bits");
            std::vector<uint8_t> floats(8);
            const float half = 0.5f, beyond = -2.0f;
            std::memcpy(floats.data(), &half, 4);
            std::memcpy(floats.data() + 4, &beyond, 4);
            const auto flt = decode_sound(wav(3, 1, 8000, 32, floats));
            check(flt.pcm == std::vector<int16_t>{16384, -32767},
                  "Float samples are scaled, and clipped at full scale");
            rejects([&] { decode_sound(std::vector<uint8_t>{'R', 'I', 'F', 'F'}); },
                    "Four bytes are not a WAV");
            rejects([&] { decode_sound(wav(1, 3, 48000, 16, pcm16({1, 2, 3}))); },
                    "Three channels are refused");
            rejects([&] { decode_sound(wav(1, 1, 4000, 16, pcm16({1}))); }, "A rate below 8 kHz is refused");
            rejects([&] { decode_sound(wav(2, 1, 48000, 16, pcm16({1}))); },
                    "A compressed WAV format is refused");
            rejects([&] { decode_sound(wav(1, 1, 48000, 16, {})); }, "A WAV with no audio is refused");
            auto cut = wav(1, 1, 48000, 16, pcm16({1, 2, 3, 4}));
            cut.resize(cut.size() - 3);
            rejects([&] { decode_sound(cut); }, "A chunk running past the end of the file is refused");
            rejects([&] { decode_sound(std::vector<uint8_t>(64, 7)); },
                    "Neither a WAV nor an Ogg is refused");
            check(audio_format(wav(1, 1, 48000, 16, pcm16({1}))) == "wav" &&
                      audio_format(std::vector<uint8_t>{}).empty(),
                  "A format is told by its first bytes");

            // Ogg Vorbis, through the pinned stb_vorbis: the fixture is half a second of 440 Hz.
            const auto ogg_bytes = read_bytes(data / "tone.ogg");
            check(audio_format(ogg_bytes) == "ogg", "The fixture is an Ogg stream");
            const auto tone = decode_sound(ogg_bytes);
            check(tone.channels == 2 && tone.rate == 44100 && tone.frames() > 21000 && tone.frames() < 23200,
                  "An Ogg Vorbis sound decodes whole: stereo, 44.1 kHz, half a second");
            int crossings = 0, peak = 0;
            for (size_t i = 1; i < tone.frames(); ++i) {
                crossings += (tone.pcm[(i - 1) * 2] < 0) != (tone.pcm[i * 2] < 0);
                peak = std::max(peak, std::abs(static_cast<int>(tone.pcm[i * 2])));
            }
            check(crossings > 400 && crossings < 480 && peak > 12000 && peak < 20000,
                  "And it is the tone that was encoded: 440 Hz at half scale");
            auto stream = open_music(ogg_bytes);
            std::vector<int16_t> streamed, chunk(1000 * 2);
            while (const size_t got = stream->read(chunk))
                streamed.insert(streamed.end(), chunk.begin(),
                                chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
            check(stream->rate() == 44100 && stream->channels() == 2 && streamed == tone.pcm,
                  "A track decoded as it plays gives exactly what decoding it whole gives");
            stream->rewind();
            check(stream->read(chunk) == 1000 && std::equal(chunk.begin(), chunk.end(), tone.pcm.begin()),
                  "And rewinds to its start");
            auto broken = ogg_bytes;
            broken.resize(200);
            rejects([&] { decode_sound(broken); }, "A truncated Ogg does not decode");
            rejects([&] { open_music(std::vector<uint8_t>(64, 7)); },
                    "A track that is neither format is refused");
        }

        // ---- Sample voices ----
        {
            Sample ramp;
            ramp.rate = 48000;
            ramp.channels = 1;
            for (int i = 0; i < 100; ++i)
                ramp.pcm.push_back(static_cast<int16_t>(i * 100));
            Mixer mixer(48000);
            Sound sound;
            sound.sample = &ramp;
            sound.gain = 1;
            check(mixer.play(sound), "A sample plays as a voice");
            std::vector<float> out(40 * 2);
            mixer.render(out);
            bool exact = true;
            for (size_t i = 0; i < 40; ++i)
                exact = exact && std::abs(out[i * 2] - static_cast<float>(ramp.pcm[i]) / 32768.0f) < 1e-6f &&
                        out[i * 2] == out[i * 2 + 1];
            check(exact, "At the mixer's rate it plays its own samples, a mono one on both sides");
            std::vector<float> rest(100 * 2);
            mixer.render(rest);
            check(mixer.active() == 0, "And the voice ends with the sample");
            check(rest[58 * 2] == static_cast<float>(ramp.pcm[98]) / 32768.0f &&
                      rest[59 * 2] == static_cast<float>(ramp.pcm[99]) / 32768.0f && rest[60 * 2] == 0,
                  "The final frame sounds too, then the sample ends without an extra frame");
            {
                Sample single;
                single.rate = 24000;
                single.channels = 2;
                single.pcm = {16384, -8192};
                Sound click;
                click.sample = &single;
                click.gain = 1;
                check(mixer.play(click), "A single stereo frame is a playable sample");
                std::array<float, 6> brief{};
                mixer.render(brief);
                check(brief == std::array<float, 6>{0.5f, -0.25f, 0.5f, -0.25f, 0, 0} && mixer.active() == 0,
                      "Its last frame is held for the source duration when resampled, then goes silent");
            }
            Sample half = ramp;
            half.rate = 24000;
            mixer.play({440, 1, 0.1f, Bus::Effects, 0, {}, false, &half});
            std::vector<float> slow(8 * 2);
            mixer.render(slow);
            check(std::abs(slow[2] - (static_cast<float>(ramp.pcm[0]) + static_cast<float>(ramp.pcm[1])) / 2 /
                                         32768.0f) < 1e-6f,
                  "A 24 kHz sample plays at half speed, interpolating between its frames");
            Sound pitched;
            pitched.sample = &ramp;
            pitched.pitch = 2;
            mixer.stop_all();
            mixer.play(pitched);
            mixer.render(slow);
            check(std::abs(slow[2] - static_cast<float>(ramp.pcm[2]) / 32768.0f) < 1e-6f,
                  "Pitch 2 plays every other frame");
            mixer.play(sound);
            mixer.forget(&ramp);
            check(mixer.active() == 0, "Forgetting a sample stops every voice playing it");
            Sound panned = sound;
            panned.positional = true;
            panned.position = {450, 0};
            mixer.set_listener({{0, 0}, 900});
            mixer.play(panned);
            mixer.render(slow);
            check(slow[6] > 0 && slow[7] > slow[6] * 1.5f, "A sample to the right is louder on the right");
            mixer.stop_all();
            mixer.set_gain(Bus::Effects, 0.5f);
            mixer.play(sound);
            mixer.render(slow);
            check(std::abs(slow[4] - static_cast<float>(ramp.pcm[2]) / 32768.0f * 0.5f) < 1e-6f,
                  "A sample obeys its bus's gain");
            Sound bad = sound;
            bad.pitch = 8;
            rejects([&] { mixer.play(bad); }, "A pitch outside 0.25..4 is refused");
            Sample empty;
            bad = sound;
            bad.sample = &empty;
            rejects([&] { mixer.play(bad); }, "A sample with no frames is refused");
        }

        // ---- Music: two decks, a crossfade, decoded ahead of the mix ----
        {
            auto constant = [](int16_t value, size_t frames, int rate = 48000) {
                auto sample = std::make_shared<Sample>();
                sample->rate = rate;
                sample->channels = 2;
                sample->pcm.assign(frames * 2, value);
                return std::make_unique<SampleStream>(sample);
            };
            Mixer mixer(48000);
            std::vector<float> out(480 * 2);
            mixer.play_music(constant(16384, 48000 * 4), 0);
            check(mixer.music() && mixer.music_decks() == 1 && mixer.music_gain() == 1,
                  "A track cut in plays at once, at its gain");
            mixer.render(out);
            check(std::abs(out[100] - 0.5f) < 1e-6f && std::abs(out[101] - 0.5f) < 1e-6f,
                  "And it is heard: a half-scale track at unit gain");
            mixer.set_gain(Bus::Music, 0.5f);
            mixer.stream_music();
            mixer.render(out);
            check(std::abs(out[100] - 0.25f) < 1e-6f, "Under the Music bus's gain");
            mixer.set_gain(Bus::Music, 1);
            // A second track crossfades in over one second while the first fades out.
            mixer.play_music(constant(8192, 48000 * 4), 1);
            check(mixer.music_decks() == 2 && mixer.music_gain() == 0, "A crossfade sounds on both decks");
            std::vector<float> half_second(24000 * 2);
            mixer.render(std::span(half_second).subspan(0, 12000 * 2));
            mixer.stream_music();
            mixer.render(std::span(half_second).subspan(12000 * 2));
            const float midway = half_second.back();
            check(std::abs(midway - (0.5f * 0.5f + 0.25f * 0.5f)) < 0.002f,
                  "Halfway through, each track is at half its gain");
            mixer.stream_music();
            mixer.render(std::span(half_second).subspan(0, 12000 * 2));
            mixer.stream_music();
            mixer.render(std::span(half_second).subspan(12000 * 2));
            check(std::abs(half_second.back() - 0.25f) < 0.002f && std::abs(mixer.music_gain() - 1) < 1e-4f,
                  "At the end only the new track is heard, at its full gain");
            mixer.stream_music();
            check(mixer.music_decks() == 1,
                  "And the old one is released - on the game thread, not in the mix");
            // A third track mid-crossfade: only two decks ever sound.
            mixer.play_music(constant(100, 48000), 1);
            mixer.render(out);
            mixer.play_music(constant(200, 48000), 1);
            check(mixer.music_decks() == 2, "A third track drops the one already fading out");
            mixer.stop_all();
            check(mixer.music_decks() == 0, "Stopping everything stops the music too");
            mixer.play_music(constant(16384, 48000), 0);
            mixer.stop_music(0.01f);
            std::vector<float> tail(4800 * 2);
            mixer.render(tail);
            check(mixer.music_decks() == 1 && !mixer.music() && tail[0] > 0.4f && tail[2 * 480] == 0,
                  "Stopping fades the current track out over its time");
            mixer.stream_music();
            check(mixer.music_decks() == 0, "And then releases it");
            // A track that does not loop plays out; one that does comes round again.
            mixer.play_music(constant(16384, 1000), 0, 1, false);
            mixer.render(tail);
            mixer.stream_music();
            check(mixer.music_decks() == 0 && std::abs(tail[1998]) > 0.4f && tail[2 * 2000] == 0,
                  "A track that does not loop plays once and stops");
            mixer.play_music(constant(16384, 1000), 0, 1, true);
            mixer.render(tail);
            check(mixer.music_decks() == 1 && std::abs(tail[2 * 3000] - 0.5f) < 1e-6f,
                  "One that loops plays on past its end");
            // A 24 kHz track at pitch on a 48 kHz mix: twice the frames out as in.
            mixer.stop_all();
            mixer.play_music(constant(16384, 100, 24000), 0, 1, false);
            std::vector<float> resampled(400 * 2);
            mixer.render(resampled);
            size_t sounding = 0;
            for (size_t i = 0; i < 400; ++i)
                sounding += resampled[i * 2] != 0;
            check(sounding >= 196 && sounding <= 200, "A 24 kHz track lasts as long on a 48 kHz mix");
            // Starved: a mix longer than what was decoded ahead is silence, counted.
            Mixer starved(48000);
            starved.play_music(constant(16384, 48000 * 4), 0);
            std::vector<float> starving((music_ring_frames + 100) * 2);
            starved.render(starving);
            check(starved.music_underruns() == 100 && starving.back() == 0,
                  "A frame longer than the ring starves the deck: silence, and counted");
            rejects([&] { mixer.play_music(nullptr, 1); }, "A track needs a stream");
            rejects([&] { mixer.play_music(constant(1, 10), 1, 2); }, "A music gain above one is refused");
            rejects([&] { mixer.play_music(constant(1, 10), -1); }, "A negative crossfade is refused");
        }

        // ---- The claim that matters: the callback allocates nothing ----
        // Measured rather than asserted. This runs on the driver's thread against a deadline, and
        // one allocation behind a lock is an audible fault rather than a slow frame.
        {
            Mixer mixer(48000);
            for (size_t i = 0; i < max_voices; ++i)
                mixer.play({440 + static_cast<float>(i), 1, 6, Bus::Effects, 0, {10, 0}, true});
            mixer.fade(Bus::Music, 0.2f, 0.5f);
            mixer.fade(Bus::Effects, 0.9f, 0.25f);
            // Two music decks crossfading, and sample voices, as a real mix has.
            auto track = std::make_shared<Sample>();
            track->channels = 2;
            track->pcm.assign(48000 * 20 * 2, 1000);
            Sample blip;
            blip.pcm.assign(48000, 2000);
            mixer.stop_all();
            for (size_t i = 0; i < max_voices; ++i)
                mixer.play(i % 2 ? Sound{440 + static_cast<float>(i), 1, 6, Bus::Effects, 0, {10, 0}, true}
                                 : Sound{0, 1, 0, Bus::Voice, 0, {10, 0}, true, &blip});
            mixer.play_music(std::make_unique<SampleStream>(track), 0);
            mixer.play_music(std::make_unique<SampleStream>(track), 5);
            std::vector<float> buffer(4800 * 2); // allocated before counting starts

            allocations = 0;
            counting = true;
            for (int pass = 0; pass < 20; ++pass) {
                mixer.render(buffer);
                mixer.stream_music(); // on the game thread; counted too, and it decodes into the ring
            }
            counting = false;
            check(allocations == 0, "Twenty full buffers, thirty-two voices, two music decks crossfading, "
                                    "two fades in flight: not one allocation");
            check(mixer.music_decks() == 2 && mixer.music_underruns() == 0,
                  "With the music genuinely playing throughout");
            check(mixer.active() > 0, "With voices genuinely playing throughout, not an empty mix");
        }

        std::cout << "PASS " << checks << " audio mixer checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " audio checks: " << e.what() << '\n';
        return 1;
    }
}
