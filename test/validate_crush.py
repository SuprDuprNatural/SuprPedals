"""Validate the Crush port/default/preset contract against the real LV2 binary.

Uses the standard library; additionally parse all RDF when rdflib is available.
Run from the repository with: make test-crush
"""
import ctypes as C
import math
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
symbols = 'in out bits rate drive env sensitivity release tone protect dry wet'.split()
ttl = (root / 'ttl/suprcrush.ttl').read_text()
cpp = (root / 'src/SuprCrush.cpp').read_text()
assert [(int(i), s) for i, s in re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"', ttl)] == list(enumerate(symbols))
assert [(int(i), s.lower()) for s, i in re.findall(r'PORT_(\w+)\s*=\s*(\d+)', cpp)] == list(enumerate(symbols))
ports = {s: (float(d), float(lo), float(hi)) for s, d, lo, hi in re.findall(
    r'lv2:symbol "([^"]+)" ; lv2:name "[^"]+" ; lv2:default ([-\d.]+) ; lv2:minimum ([-\d.]+) ; lv2:maximum ([-\d.]+)', ttl)}
assert len(ports) == 10
assert 'pprops:rangeSteps' not in ttl
assert all(ports[s][1:] == (-60, 24) for s in ('dry', 'wet'))
assert 'units:render "%f oct"' in ttl
presets = (root / 'ttl/crush-presets.ttl').read_text()
manifest = (root / 'ttl/crush-manifest.ttl').read_text()
assert '<suprcrush.so>' in manifest and '<suprcrush.ttl>' in manifest
ids = lambda text: re.findall(r'<([^>]+)> a pset:Preset', text)
assert ids(presets) == ids(manifest) and len(ids(presets)) == 4
for block in presets.split('a pset:Preset')[1:]:
    values = re.findall(r'lv2:symbol "([^"]+)" ; pset:value ([-\d.]+)', block)
    assert [s for s, v in values] == symbols[2:]
    for s, value in values:
        value = float(value)
        assert ports[s][1] <= value <= ports[s][2], (s, value)
        if s in ('protect',):
            assert value == round(value)
try:
    import rdflib
except ImportError:
    print('RDF syntax parser unavailable; standard-library contract checks remain active.')
else:
    for file in ['suprcrush.ttl', 'crush-presets.ttl', 'crush-manifest.ttl']:
        rdflib.Graph().parse(root / 'ttl' / file, format='turtle')
    print('PASS full RDF syntax')

class Descriptor(C.Structure):
    pass
Instantiate = C.CFUNCTYPE(C.c_void_p, C.POINTER(Descriptor), C.c_double, C.c_char_p, C.c_void_p)
Connect = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32, C.c_void_p)
Activate = C.CFUNCTYPE(None, C.c_void_p)
Run = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32)
Descriptor._fields_ = [('uri', C.c_char_p), ('instantiate', Instantiate), ('connect', Connect),
                      ('activate', Activate), ('run', Run), ('deactivate', C.c_void_p),
                      ('cleanup', Activate), ('extension', C.c_void_p)]
lib = C.CDLL(str(root / 'build/suprcrush.so'))
lib.lv2_descriptor.argtypes = [C.c_uint32]
lib.lv2_descriptor.restype = C.POINTER(Descriptor)
dp = lib.lv2_descriptor(0)
desc = dp.contents
assert desc.uri.decode() == 'https://suprduprnatural.github.io/supr-pedals/crush'
assert not lib.lv2_descriptor(1)

def render(values):
    n = 12000
    inp = (C.c_float * n)(*(.2 * math.sin(i * 2 * math.pi * 110 / 48000) for i in range(n)))
    out = (C.c_float * n)()
    handle = desc.instantiate(dp, 48000, None, None)
    assert handle
    cv = {s: C.c_float(v) for s, v in values.items()}
    try:
        for s, v in cv.items():
            desc.connect(handle, symbols.index(s), C.byref(v))
        desc.connect(handle, 0, inp)
        desc.connect(handle, 1, out)
        desc.activate(handle)
        desc.run(handle, n)
        assert all(math.isfinite(v) for v in out)
        return bytes(out)
    finally:
        desc.cleanup(handle)

assert render({}) == render({s: ports[s][0] for s in symbols[2:]}), 'DSP defaults differ from TTL'
for block in presets.split('a pset:Preset')[1:]:
    render({s: float(v) for s, v in re.findall(r'lv2:symbol "([^"]+)" ; pset:value ([-\d.]+)', block)})
print('PASS 12 ports, enum/TTL agreement, actual binary/defaults, four complete bounded presets')
