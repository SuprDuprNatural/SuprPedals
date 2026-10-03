"""Check full RDF syntax when rdflib is installed, plus exact new port contracts."""
import re
from pathlib import Path
root = Path(__file__).resolve().parents[1]
contracts = {
    'echo': 'in out time feedback dry duck tone lowcut recovery division send hold duck_gr wet',
    'space': 'in out dry decay tone duck predelay lowcut recovery send duck_gr wet',
}
try:
    import rdflib
except ImportError:
    rdflib = None
for effect, names in contracts.items():
    expected = list(enumerate(names.split()))
    ttl = (root / f'ttl/supr{effect}.ttl').read_text()
    cpp = (root / f'src/Supr{effect.capitalize()}.cpp').read_text()
    assert [(int(i), s) for i, s in re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"', ttl)] == expected
    assert [(int(i), s.lower()) for s, i in re.findall(r'PORT_(\w+)\s*=\s*(\d+)', cpp)] == expected
    for file in [f'supr{effect}.ttl', f'{effect}-presets.ttl', f'{effect}-manifest.ttl']:
        if rdflib:
            rdflib.Graph().parse(root / 'ttl' / file, format='turtle')
    preset = (root / f'ttl/{effect}-presets.ttl').read_text()
    for block in preset.split('a pset:Preset')[1:]:
        assert re.findall(r'lv2:symbol "([^"]+)"', block) == [s for s in names.split()[2:] if s != 'duck_gr']
    assert f'supr{effect}.so' in (root / f'ttl/{effect}-manifest.ttl').read_text()
    print(f'PASS {effect}: enum/TTL/presets agree ({len(expected)} ports); RDF {"parsed" if rdflib else "unavailable"}')
