"""Renders the game's synthesized sound effects and music loops to WAV files in sounds/.

The original web build synthesizes everything live with WebAudio oscillators; this script
plays the same note lists through the same envelopes offline. Run from the project folder:

    python tools/make_audio.py
"""
import math
import os
import random
import struct
import wave

import numpy as np

SR = 32000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sounds")
MASTER = 0.8
rnd = random.Random(7)
NOISE = np.random.default_rng(7).uniform(-1, 1, int(SR * 1.5))


class Buf:
    def __init__(self, seconds):
        self.data = np.zeros(int(SR * seconds) + 1)

    def add(self, t, samples):
        i = int(round(t * SR))
        if i >= len(self.data):
            return
        n = min(len(samples), len(self.data) - i)
        self.data[i:i + n] += samples[:n]


def biquad(x, kind, freq, q):
    """RBJ biquad with a per-sample frequency (array or scalar), like a BiquadFilterNode."""
    freq = np.broadcast_to(np.asarray(freq, dtype=float), x.shape)
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    last = None
    b0 = b1 = b2 = a1 = a2 = 0.0
    for i in range(len(x)):
        f = freq[i]
        if f != last:
            last = f
            w = 2 * math.pi * min(f, SR * 0.45) / SR
            cs, sn = math.cos(w), math.sin(w)
            alpha = sn / (2 * q)
            if kind == "lowpass":
                b0, b1, b2 = (1 - cs) / 2, 1 - cs, (1 - cs) / 2
            else:  # bandpass, constant 0 dB peak gain
                b0, b1, b2 = alpha, 0.0, -alpha
            a0 = 1 + alpha
            b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, -2 * cs / a0, (1 - alpha) / a0
        v = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, x[i], y1, v
        y[i] = v
    return y


def envelope(n, d, v, a):
    """Linear attack to v over `a`, then exponential decay to silence at `d`."""
    t = np.arange(n) / SR
    a = min(a, d * 0.8)
    env = np.where(t < a, 0.0001 + (v - 0.0001) * t / max(a, 1e-6),
                   v * (0.0001 / v) ** np.clip((t - a) / max(d - a, 1e-6), 0, 1))
    env[t > d] = 0
    return env


def tone(buf, f, t, d, type="sine", v=0.1, a=0.005, to=None, lp=None, det=0):
    n = int((d + 0.02) * SR)
    ts = np.arange(n) / SR
    f0 = f * 2 ** (det / 1200)
    if to:
        freq = f0 * (to / f) ** np.clip(ts / d, 0, 1)
    else:
        freq = np.full(n, f0)
    phase = np.cumsum(freq) / SR
    if type == "sine":
        osc = np.sin(2 * math.pi * phase)
    elif type == "triangle":
        osc = 2 * np.abs(2 * (phase - np.floor(phase + 0.5))) - 1
    else:  # sawtooth
        osc = 2 * (phase - np.floor(phase + 0.5))
    if lp:
        osc = biquad(osc, "lowpass", lp, 1.0)
    buf.add(t, osc * envelope(n, d, v, a))


def noise(buf, t, d, v=0.1, a=0.003, f=1500, to=None, q=0.8, type="bandpass"):
    n = int((d + 0.02) * SR)
    ts = np.arange(n) / SR
    start = rnd.randrange(0, len(NOISE) - n)
    src = NOISE[start:start + n]
    freq = f * (to / f) ** np.clip(ts / d, 0, 1) if to else f
    buf.add(t, biquad(src, type, freq, q) * envelope(n, d, v, a))


def hz(m):
    return 440 * 2 ** ((m - 69) / 12)


PENT = [0, 2, 4, 7, 9]


def deg(i):
    return 60 + 12 * (i // 5) + PENT[i % 5]


# ---------------------------------------------------------------- sound effects

def sfx_shoot(b):
    tone(b, 880, 0, 0.07, v=0.03, to=1150)


def sfx_hit(b):
    tone(b, 340, 0, 0.09, type="triangle", v=0.06, to=220)


def sfx_kill(b):
    tone(b, 250, 0, 0.09, v=0.07, to=700)
    tone(b, 500, 0.05, 0.08, v=0.03, to=1100)


def sfx_pickup(b):  # the game raises the pitch for quick pickups in a row
    f = 784
    tone(b, f, 0, 0.2, v=0.05)
    tone(b, f * 1.5, 0.04, 0.18, v=0.03)
    tone(b, f * 2, 0.04, 0.12, v=0.012)


def sfx_oil(b):
    tone(b, 200, 0, 0.11, v=0.14, to=420, lp=1200)
    tone(b, 260, 0.09, 0.13, v=0.12, to=540, lp=1200)


def sfx_levelup(b):
    for i, f in enumerate([523, 659, 784, 1047, 1319]):
        tone(b, f, i * 0.09, 0.4, type="triangle", v=0.1)
        tone(b, f, i * 0.09, 0.3, v=0.05)


def sfx_select(b):
    tone(b, 1047, 0, 0.4, v=0.09)
    tone(b, 2093, 0, 0.2, v=0.025)


def sfx_hurt(b):
    tone(b, 200, 0, 0.2, v=0.3, to=85, lp=500)
    tone(b, 130, 0, 0.18, type="triangle", v=0.15, lp=400)


def sfx_brazier(b):
    noise(b, 0, 0.6, v=0.1, a=0.2, f=250, to=1500, q=0.5)
    for i, f in enumerate([262, 330, 392, 523]):
        tone(b, f, 0.12 + i * 0.03, 1.3, type="triangle", v=0.07, a=0.08)


def sfx_boss(b):
    def wah(f, to, at, d):
        tone(b, f, at, d, type="sawtooth", v=0.1, a=0.04, to=to, lp=600)
        tone(b, f, at, d, type="triangle", v=0.1, a=0.04, to=to)

    wah(196, 185, 0, 0.3)
    wah(175, 131, 0.33, 0.55)


def sfx_chest(b):
    for i in range(10):
        tone(b, hz(72 + 12 * (i // 5) + PENT[i % 5]), i * 0.055, 0.28, v=0.06)


def sfx_evolve(b):
    for i, f in enumerate([262, 330, 392, 523, 659, 784]):
        tone(b, f, i * 0.05, 1.9, type="triangle", v=0.06, a=0.15)
        tone(b, f, i * 0.05, 1.9, v=0.035, a=0.2, det=10)
    for i in range(10):
        tone(b, 1500 + rnd.random() * 2000, 0.25 + i * 0.1, 0.22, v=0.02)


def sfx_death(b):
    for i, f in enumerate([523, 440, 349]):
        tone(b, f, i * 0.24, 0.45, type="triangle", v=0.1, to=f * 0.94, lp=1500)


def sfx_victory(b):
    for f, dt, d in [(523, 0, 0.12), (523, 0.14, 0.12), (523, 0.28, 0.12), (659, 0.46, 0.2), (784, 0.72, 0.3)]:
        tone(b, f, dt, d + 0.1, type="triangle", v=0.11)
        tone(b, f * 2, dt, d + 0.1, v=0.03)
    for f in [523, 659, 784, 1047]:
        tone(b, f, 1.05, 1.5, type="triangle", v=0.08, a=0.03)


def sfx_click(b):
    tone(b, 1000, 0, 0.025, v=0.04, to=700)


def sfx_buy(b):
    tone(b, 1319, 0, 0.09, v=0.07)
    tone(b, 1760, 0.08, 0.35, v=0.07)
    tone(b, 2637, 0.08, 0.2, v=0.015)


def sfx_deny(b):
    tone(b, 196, 0, 0.1, type="triangle", v=0.12, to=165, lp=700)
    tone(b, 196, 0.13, 0.12, type="triangle", v=0.12, to=160, lp=700)


def sfx_pulse(b):
    tone(b, 110, 0, 0.45, v=0.4, to=45)
    noise(b, 0, 0.2, v=0.04, f=250, type="lowpass")


def sfx_zap(b):
    tone(b, 600, 0, 0.1, v=0.035, to=2400)
    tone(b, 900, 0.03, 0.08, type="triangle", v=0.02, to=3000)


def sfx_flask(b):
    tone(b, 1800, 0, 0.15, v=0.06)
    tone(b, 2400, 0.04, 0.12, v=0.04)
    noise(b, 0.06, 0.3, v=0.04, a=0.05, f=2500, to=1200, q=0.5)


SFX = {
    "shoot": (sfx_shoot, 0.12), "hit": (sfx_hit, 0.14), "kill": (sfx_kill, 0.18), "pickup": (sfx_pickup, 0.26),
    "oil": (sfx_oil, 0.26), "levelup": (sfx_levelup, 0.8), "select": (sfx_select, 0.45), "hurt": (sfx_hurt, 0.25),
    "brazier": (sfx_brazier, 1.6), "boss": (sfx_boss, 0.95), "chest": (sfx_chest, 0.85), "evolve": (sfx_evolve, 2.2),
    "death": (sfx_death, 1.0), "victory": (sfx_victory, 2.6), "click": (sfx_click, 0.06), "buy": (sfx_buy, 0.48),
    "deny": (sfx_deny, 0.3), "pulse": (sfx_pulse, 0.5), "zap": (sfx_zap, 0.15), "flask": (sfx_flask, 0.4),
}

# ---------------------------------------------------------------- music

C, G, F, Am = (48, False), (43, False), (41, False), (45, True)


def chord_tones(ch):
    root, minor = ch
    return [root + i for i in (0, 3 if minor else 4, 7, 12)]


def box(b, m, t, d, v):
    tone(b, hz(m), t, d, v=v)
    tone(b, hz(m) * 2, t, d * 0.4, type="triangle", v=v * 0.3)
    tone(b, hz(m), t, d, v=v * 0.4, det=8)


def melody(b, mel, i, t, d, v):
    if mel[i] >= 0:
        box(b, deg(mel[i]), t, d, v)


def tick(b, t):
    tone(b, 1800, t, 0.02, v=0.012)


def play_menu(b, i, t, ch, mel):
    melody(b, mel, i, t, 1.6, 0.07)
    if i % 2 == 0:
        box(b, chord_tones(ch)[[0, 2, 1, 3][(i // 2) % 4]] + 12, t, 1.2, 0.025)
    if i == 0:
        tone(b, hz(ch[0]), t, 2.6, type="triangle", v=0.09, a=0.05, lp=500)
    if rnd.random() < 0.12:
        box(b, deg(10 + rnd.randrange(3)), t + 0.05, 1.8, 0.025)


def play_run(b, i, t, ch, mel):
    melody(b, mel, i, t, 0.45, 0.075)
    ct = chord_tones(ch)
    if i % 4 == 0:
        tone(b, hz(ch[0]), t, 0.28, type="triangle", v=0.13, lp=600)
    if i % 4 == 2:
        tone(b, hz(ct[2]), t, 0.2, type="triangle", v=0.09, lp=600)
    if i == 4:
        for m in ct[:3]:
            box(b, m + 12, t, 0.5, 0.02)
    if i % 2 == 1:
        tick(b, t)


def play_boss(b, i, t, ch, mel):
    melody(b, mel, i, t, 0.2, 0.07)
    ct = chord_tones(ch)
    if i % 2 == 0:
        tone(b, hz(ch[0]), t, 0.16, type="triangle", v=0.14, lp=600)
    else:
        tone(b, hz(ct[2] + 12), t, 0.1, type="triangle", v=0.05, lp=900)
        tick(b, t)


TRACKS = {
    "menu": (60 / 80 / 2, play_menu, 2, [
        (C, [7, -1, 8, -1, 7, -1, 5, -1]), (Am, [4, -1, 5, -1, 7, -1, -1, -1]),
        (F, [5, -1, 4, -1, 5, -1, 7, -1]), (G, [6, -1, 3, -1, 6, -1, -1, -1])]),
    "run": (60 / 112 / 2, play_run, 1, [
        (C, [5, -1, 7, 8, 7, -1, 6, -1]), (G, [5, -1, 6, 7, 6, -1, 3, -1]),
        (Am, [4, -1, 5, 6, 7, -1, 6, 5]), (F, [6, -1, 5, 4, 5, -1, -1, -1]),
        (C, [5, -1, 7, 8, 9, -1, 8, 7]), (G, [8, -1, 7, 6, 7, -1, 5, -1]),
        (F, [4, 5, 6, -1, 7, 6, 5, -1]), (G, [6, -1, 3, 4, 5, -1, -1, 8])]),
    "boss": (60 / 138 / 2, play_boss, 1, [
        (C, [5, 7, 5, 7, 8, -1, 7, -1]), (C, [8, 7, 8, 9, 10, -1, 8, -1]),
        (F, [6, 5, 6, 7, 6, -1, 4, -1]), (G, [7, 6, 5, 6, 3, -1, -1, 6]),
        (C, [5, 7, 5, 7, 8, -1, 9, -1]), (Am, [9, 8, 7, 8, 7, -1, 4, -1]),
        (F, [6, 7, 8, 7, 6, 5, 4, -1]), (G, [5, -1, 6, -1, 7, -1, 8, 9])]),
}


def render_track(name):
    step, play, repeats, bars = TRACKS[name]
    length = step * 8 * len(bars) * repeats
    b = Buf(length + 4)
    n = 0
    for _ in range(repeats):
        for ch, mel in bars:
            for i in range(8):
                play(b, i, n * step, ch, mel)
                n += 1
    # fold the tail back onto the start so the loop is seamless
    loop = int(round(length * SR))
    out = b.data[:loop].copy()
    tail = b.data[loop:]
    out[:len(tail)] += tail[:loop]
    return out


def write(name, samples):
    os.makedirs(OUT, exist_ok=True)
    pcm = np.tanh(samples * MASTER * 1.6) * 32767  # gentle limiter standing in for the compressor
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack("<%dh" % len(pcm), *pcm.astype(np.int16)))


def main():
    for name, (fn, seconds) in SFX.items():
        b = Buf(seconds)
        fn(b)
        fade = np.minimum(1, np.arange(len(b.data))[::-1] / (0.01 * SR))
        write(name, b.data * fade)
    for name in TRACKS:
        write("music_" + name, render_track(name))
    print("wrote", len(os.listdir(OUT)), "files to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
