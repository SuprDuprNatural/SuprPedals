"""Audition the actual SuprVowel LV2 binary and factory presets on a mono PCM16 DI.

python3 tools/render_vowel.py input.wav build/vowel/auditions
No normalization or automatic level matching. Writes dry, six presets, and an
Envelope -> Vowel example; refuses to write clipped PCM. Private audio stays
under the ignored build directory when using the command above.
"""
import array
import ctypes as C
import math
from pathlib import Path
import re
import sys
import wave

root = Path(__file__).resolve().parents[1]
source, destination = map(Path, sys.argv[1:3])
destination.mkdir(parents=True, exist_ok=True)
with wave.open(str(source)) as w:
    assert w.getnchannels() == 1 and w.getsampwidth() == 2, 'Expected mono PCM16 WAV'
    sr = w.getframerate()
    samples = array.array('h', w.readframes(w.getnframes()))
    if sys.byteorder != 'little':
        samples.byteswap()
audio = [v / 32768 for v in samples] + [0.] * sr

class Descriptor(C.Structure):
    pass
Instantiate = C.CFUNCTYPE(C.c_void_p, C.POINTER(Descriptor), C.c_double, C.c_char_p, C.c_void_p)
Connect = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32, C.c_void_p)
Activate = C.CFUNCTYPE(None, C.c_void_p)
Run = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32)
Descriptor._fields_ = [('uri', C.c_char_p), ('instantiate', Instantiate), ('connect', Connect),
                      ('activate', Activate), ('run', Run), ('deactivate', C.c_void_p),
                      ('cleanup', Activate), ('extension', C.c_void_p)]

def render(effect, values, audio):
    lib = C.CDLL(str(root / f'build/supr{effect}.so'))
    lib.lv2_descriptor.argtypes = [C.c_uint32]
    lib.lv2_descriptor.restype = C.POINTER(Descriptor)
    dp = lib.lv2_descriptor(0)
    d = dp.contents
    ttl = (root / f'ttl/supr{effect}.ttl').read_text()
    ports = {s: int(i) for i, s in re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"', ttl)}
    controls = {s: C.c_float(v) for s, v in values.items()}
    handle = d.instantiate(dp, sr, None, None)
    assert handle
    inp, out = (C.c_float * 128)(), (C.c_float * 128)()
    result = array.array('f')
    try:
        for s, v in controls.items():
            d.connect(handle, ports[s], C.byref(v))
        d.connect(handle, 0, inp)
        d.connect(handle, 1, out)
        d.activate(handle)
        for offset in range(0, len(audio), 128):
            n = min(128, len(audio) - offset)
            for i in range(n):
                inp[i] = audio[offset + i]
            d.run(handle, n)
            result.extend(out[:n])
        assert all(math.isfinite(v) for v in result)
        return result
    finally:
        d.cleanup(handle)

def save(name, values):
    peak = max(abs(v) for v in values)
    assert peak < 1, f'{name} clips PCM ({peak:.3f}); lower the source or preset Level explicitly'
    pcm = array.array('h', (max(-32768, min(32767, round(v * 32768))) for v in values))
    if sys.byteorder != 'little':
        pcm.byteswap()
    with wave.open(str(destination / (name + '.wav')), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(pcm.tobytes())
    rms = math.sqrt(sum(v * v for v in values) / len(values))
    print(f'{name}: peak {20 * math.log10(max(peak, 1e-12)):.2f} dBFS, '
          f'RMS {20 * math.log10(max(rms, 1e-12)):.2f} dBFS; no normalization/clipping')

save('00-dry', audio)
for i, block in enumerate((root / 'ttl/vowel-presets.ttl').read_text().split('a pset:Preset')[1:], 1):
    label = re.search(r'rdfs:label "([^"]+)"', block)[1]
    values = {s: float(v) for s, v in re.findall(r'lv2:symbol "([^"]+)" ; pset:value ([-\d.]+)', block)}
    save(f'{i:02}-{label.lower().replace(" ", "-")}', render('vowel', values, audio))
    if label == 'Envelope Companion':
        # Existing Envelope defaults, with its blend reduced for more harmonic
        # material. LFO Vowel keeps the two sweeps independently expressive.
        env = render('envfilter', {'blend': .55}, audio)
        save('07-envelope-then-vowel', render('vowel', values, env))
