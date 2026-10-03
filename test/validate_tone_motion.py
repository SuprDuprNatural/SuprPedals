"""Validate the Phase contract, all preset values, and RDF when available."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
try:
 import rdflib
except ImportError:
 rdflib=None
contracts={'phase':'in out rate depth centre feedback dry protect mode sensitivity wet'}
for effect,names in contracts.items():
 symbols=names.split();ttl=(root/f'ttl/supr{effect}.ttl').read_text();cpp=(root/f'src/Supr{effect.title()}.cpp').read_text()
 expected=list(enumerate(symbols))
 assert [(int(i),s) for i,s in re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"',ttl)]==expected
 assert [(int(i),s.lower()) for s,i in re.findall(r'PORT_(\w+)\s*=\s*(\d+)',cpp)]==expected
 limits={s:(float(lo),float(hi)) for s,lo,hi in re.findall(r'lv2:symbol "([^"]+)"[^\]]*?lv2:minimum ([-\d.]+) ; lv2:maximum ([-\d.]+)',ttl)}
 controls=symbols[2:]
 presets=(root/f'ttl/{effect}-presets.ttl').read_text()
 for block in presets.split('a pset:Preset')[1:]:
  values=re.findall(r'lv2:symbol "([^"]+)" ; pset:value ([-\d.]+)',block)
  assert [s for s,v in values]==controls
  for s,v in values:assert limits[s][0]<=float(v)<=limits[s][1],(s,v)
 manifest=(root/f'ttl/{effect}-manifest.ttl').read_text()
 assert f'supr{effect}.so' in manifest
 assert manifest.count('a pset:Preset')==presets.count('a pset:Preset')
 for file in [f'supr{effect}.ttl',f'{effect}-presets.ttl',f'{effect}-manifest.ttl']:
  if rdflib:rdflib.Graph().parse(root/'ttl'/file,format='turtle')
 print(f'PASS {effect}: {len(symbols)} ports, enum/TTL/presets/ranges agree; RDF {"parsed" if rdflib else "unavailable (install rdflib for full syntax gate)"}')
