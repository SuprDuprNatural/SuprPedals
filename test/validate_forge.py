"""Dependency-free check of Forge's metadata/enum contract; RDF parsing is additional QA."""
import re
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ttl = (root / 'ttl/suprforge.ttl').read_text()
cpp = (root / 'src/SuprForge.cpp').read_text()
expected = ['in','out','drive','weight','tight','bite','fizz','level','comp','gate','low_gr','gate_gr','peak','latency']
ports = re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"', ttl)
assert [(int(i),s) for i,s in ports] == list(enumerate(expected)), ports
enum = re.findall(r'PORT_(\w+)\s*=\s*(\d+)', cpp)
assert [(int(i),s.lower()) for s,i in enum] == list(enumerate(expected)), enum
assert 'lv2:designation lv2:latency' in ttl
assert 'suprforge.so' in (root / 'ttl/forge-manifest.ttl').read_text()
print('PASS Forge TTL: 14 contiguous ports agree with wrapper enum')
