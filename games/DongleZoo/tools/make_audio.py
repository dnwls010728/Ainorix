"""Synthesizes the game's sound effects and its music loop into sounds/*.wav.
Run from the project folder:

    python tools/make_audio.py
"""
import os
import wave

import numpy as np

SR = 32000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sounds")
rng = np.random.default_rng(11)


def t(seconds):
    return np.arange(int(SR * seconds)) / SR


def env(n, attack=0.005, decay=6.0):
    """Fast attack, exponential decay."""
    x = np.arange(n) / SR
    return np.minimum(1, x / attack) * np.exp(-x * decay)


def tone(freq, seconds, decay=6.0, kind="sine", attack=0.005, vibrato=0.0):
    x = t(seconds)
    f = freq * (1 + vibrato * np.sin(2 * np.pi * 6 * x))
    ph = 2 * np.pi * np.cumsum(f) / SR
    if kind == "sine":
        w = np.sin(ph)
    elif kind == "tri":
        w = 2 / np.pi * np.arcsin(np.sin(ph))
    elif kind == "soft":  # sine with a little second and third harmonic: marimba-ish
        w = np.sin(ph) + 0.35 * np.sin(2 * ph) * np.exp(-x * 14) + 0.2 * np.sin(4 * ph) * np.exp(-x * 30)
    else:
        w = np.sign(np.sin(ph)) * 0.5
    return w * env(len(x), attack, decay)


def sweep(f0, f1, seconds, decay=8.0, kind="sine"):
    x = t(seconds)
    f = f0 * (f1 / f0) ** (x / seconds)
    ph = 2 * np.pi * np.cumsum(f) / SR
    w = np.sin(ph) if kind == "sine" else 2 / np.pi * np.arcsin(np.sin(ph))
    return w * env(len(x), 0.004, decay)


def noise(seconds, decay=20.0, lowpass=0.2):
    n = int(SR * seconds)
    x = rng.uniform(-1, 1, n)
    y = np.zeros(n)
    acc = 0.0
    for i in range(n):
        acc += lowpass * (x[i] - acc)
        y[i] = acc
    return y * env(n, 0.002, decay)


def mix(seconds, *parts):
    """parts = (start seconds, samples, gain)."""
    out = np.zeros(int(SR * seconds) + 1)
    for start, samples, gain in parts:
        i = int(start * SR)
        n = min(len(samples), len(out) - i)
        if n > 0:
            out[i:i + n] += samples[:n] * gain
    return out


def write(name, data, peak=0.85):
    os.makedirs(OUT, exist_ok=True)
    data = np.asarray(data, dtype=float)
    m = np.max(np.abs(data))
    if m > 0:
        data = data / m * peak
    fade = min(len(data), int(SR * 0.01))
    data[-fade:] *= np.linspace(1, 0, fade)
    pcm = (data * 32767).astype("<i2")
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes(pcm.tobytes())


def note(n):
    """MIDI note number -> Hz."""
    return 440.0 * 2 ** ((n - 69) / 12)


def sfx():
    # drop: a soft "boop"
    write("drop", mix(0.2, (0, sweep(520, 300, 0.16, 16), 1), (0, noise(0.05, 60, 0.3), 0.25)))
    # merge: a bubbly pop with a bright ping; the game raises the pitch with the tier and combo
    write("merge", mix(0.4, (0, sweep(300, 900, 0.09, 14), 0.9), (0.03, tone(note(84), 0.34, 10, "soft"), 0.7),
                       (0.03, tone(note(91), 0.3, 12, "soft"), 0.35), (0, noise(0.04, 70, 0.5), 0.3)))
    # big merge (panda and above): a chord sparkle
    write("merge_big", mix(0.9, (0, sweep(200, 700, 0.12, 10), 0.8), (0.04, tone(note(72), 0.8, 5, "soft"), 0.6),
                           (0.10, tone(note(76), 0.7, 5, "soft"), 0.55), (0.16, tone(note(79), 0.7, 5, "soft"), 0.5),
                           (0.22, tone(note(84), 0.6, 5, "soft"), 0.5)))
    write("click", mix(0.08, (0, tone(900, 0.06, 50, "tri"), 1), (0, noise(0.02, 120, 0.6), 0.3)))
    write("deny", mix(0.25, (0, tone(196, 0.1, 18, "square"), 0.8), (0.11, tone(156, 0.13, 16, "square"), 0.8)))
    write("coin", mix(0.4, (0, tone(note(88), 0.1, 18, "soft"), 0.8), (0.07, tone(note(93), 0.3, 9, "soft"), 0.9)))
    write("item", mix(0.45, (0, sweep(400, 1600, 0.35, 5, "tri"), 0.8), (0.05, noise(0.3, 9, 0.5), 0.25)))
    write("hammer", mix(0.3, (0, sweep(260, 70, 0.2, 16), 1), (0, noise(0.1, 30, 0.25), 0.7)))
    write("shake", mix(0.5, *[(i * 0.07, noise(0.08, 35, 0.35), 0.8) for i in range(6)], (0, sweep(140, 220, 0.45, 5), 0.4)))
    write("warn", mix(0.3, (0, tone(note(81), 0.12, 14, "tri"), 0.8), (0.14, tone(note(81), 0.12, 14, "tri"), 0.8)))
    write("gameover", mix(1.3, (0, tone(note(72), 0.3, 6, "soft"), 0.8), (0.2, tone(note(69), 0.3, 6, "soft"), 0.8),
                          (0.4, tone(note(65), 0.3, 6, "soft"), 0.8), (0.6, tone(note(60), 0.7, 4, "soft", vibrato=0.01), 0.9)))
    write("fanfare", mix(1.4, *[(i * 0.11, tone(note(n), 0.5, 6, "soft"), 0.8) for i, n in enumerate((72, 76, 79, 84, 88))],
                         (0.55, tone(note(84), 0.8, 3.5, "soft"), 0.6), (0.55, tone(note(88), 0.8, 3.5, "soft"), 0.5),
                         (0.55, tone(note(91), 0.8, 3.5, "soft"), 0.5)))
    write("gift", mix(0.9, *[(i * 0.08, tone(note(n), 0.4, 8, "soft"), 0.8) for i, n in enumerate((79, 84, 86, 91))]))
    write("pop", mix(0.15, (0, sweep(700, 1300, 0.07, 22), 1), (0, noise(0.03, 90, 0.6), 0.3)))


def music():
    """A light 16-bar loop: plucked melody, soft bass and an offbeat chord."""
    bpm = 108
    beat = 60 / bpm
    bars = 16
    total = bars * 4 * beat
    out = np.zeros(int(SR * total) + 1)

    def put(start, samples, gain):
        i = int(start * SR)
        n = len(samples)
        if i + n <= len(out):
            out[i:i + n] += samples * gain
        else:  # wrap the tail around so the loop has no seam
            k = len(out) - i
            out[i:] += samples[:k] * gain
            out[:n - k] += samples[k:] * gain

    chords = [(60, 64, 67), (57, 60, 64), (53, 57, 60), (55, 59, 62)] * 4  # C Am F G
    melody = [
        [76, None, 79, None, 84, None, 79, 76], [72, None, 76, None, 81, None, 76, None],
        [77, None, 81, None, 84, None, 81, 77], [79, None, 83, None, 86, 83, 79, None],
        [84, None, 83, 84, 79, None, 76, None], [81, None, 79, 81, 76, None, 72, None],
        [77, 79, 81, None, 84, None, 81, None], [79, None, 86, None, 83, None, 79, None],
    ] * 2
    for bar in range(bars):
        t0 = bar * 4 * beat
        root = chords[bar][0]
        for b, n in ((0, root - 24), (1.5, root - 24), (2, root - 17), (3, root - 24)):
            put(t0 + b * beat, tone(note(n), 0.5, 5, "tri", 0.01), 0.5)
        for b in (1, 3):
            for n in chords[bar]:
                put(t0 + b * beat, tone(note(n), 0.35, 9, "soft"), 0.13)
        for i, n in enumerate(melody[bar]):
            if n:
                put(t0 + i * beat / 2, tone(note(n), 0.6, 6, "soft"), 0.34)
        for b in range(8):
            put(t0 + b * beat / 2, noise(0.04, 90, 0.9), 0.05 if b % 2 else 0.025)

    m = np.max(np.abs(out))
    pcm = (out / m * 0.6 * 32767).astype("<i2")
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, "music.wav"), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes(pcm.tobytes())


if __name__ == "__main__":
    sfx()
    music()
    print("audio written")
