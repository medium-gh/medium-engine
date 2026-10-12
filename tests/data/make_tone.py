#!/usr/bin/env python3
"""Regenerates tone.ogg, the audio suite's Ogg Vorbis fixture: half a second of a 440 Hz sine at
-6 dBFS, stereo, 44.1 kHz, encoded by oggenc (vorbis-tools) at quality 2. The suite decodes it and
checks the shape and the pitch, not exact samples, so a different encoder build still passes."""
import math, struct, subprocess, sys, tempfile, wave
from pathlib import Path

here = Path(__file__).resolve().parent
rate, seconds = 44100, 0.5
with tempfile.TemporaryDirectory() as temp:
    source = Path(temp) / "tone.wav"
    with wave.open(str(source), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * seconds)):
            value = int(16384 * math.sin(2 * math.pi * 440 * i / rate))
            frames += struct.pack("<hh", value, value)
        out.writeframes(bytes(frames))
    subprocess.run(["oggenc", "-Q", "-q", "2", "--serial", "20260924", "-o", str(here / "tone.ogg"), str(source)],
                   check=True)
print(f"wrote {here / 'tone.ogg'}", file=sys.stderr)
