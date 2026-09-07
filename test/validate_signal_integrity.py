"""Exact saved-port contracts for the signal-integrity repairs."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
CONTRACTS = {
    'Band': (
        'IN OUT SPLIT1 SPLIT2 LO_DRIVE LO_COMP LO_LEVEL MD_DRIVE MD_COMP MD_LEVEL HI_DRIVE HI_COMP HI_LEVEL BLEND LEVEL GR_LO GR_MD GR_HI LATENCY DRV_LO DRV_MD DRV_HI',
        'in out split1 split2 lowDrive lowComp lowLevel midDrive midComp midLevel highDrive highComp highLevel blend level grLow grMid grHigh latency drvLow drvMid drvHigh'),
    'Compressor': ('IN OUT THRESHOLD RATIO ATTACK RELEASE SCHPF MAKEUP BLEND GR DETECTOR',
                   'in out threshold ratio attack release schpf makeup blend gr detector'),
    'Clack': ('IN OUT CLACK SCRAPE SENSE FOCUS THRESH RANGE RELEASE LATENCY CLACKGR SCRGR EXPGR ENV DELTA LEARN LEARNED LEARN_STATE SIEVE_STATE',
              'in out clack scrape sense focus thresh range release latency clackgr scrapegr expgr env delta learn learned learn_state sieve_state'),
    'Fuzz': ('IN OUT SUSTAIN TONE GATE BLEND LEVEL NOTE LATENCY MATCH_MODE HELD_GAIN LEARN LEARNED_GAIN LEARN_STATE',
             'in out sustain tone gate blend level note latency match_mode held_gain learn learned_gain learn_state'),
}
try:
    import rdflib
except ImportError:
    rdflib = None
for name, (enums, symbols) in CONTRACTS.items():
    source = (ROOT / f'src/Supr{name}.cpp').read_text()
    body = re.search(r'enum PortIndex[^\{]*\{(.*?)\};', source, re.S).group(1)
    actual = sorted((int(i), s) for s, i in re.findall(r'PORT_(\w+)\s*=\s*(\d+)', body))
    assert actual == list(enumerate(enums.split())), (name, actual)
    path = ROOT / f'ttl/supr{name.lower()}.ttl'
    ttl = path.read_text()
    ports = sorted((int(i), s) for i, s in re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"', ttl))
    assert ports == list(enumerate(symbols.split())), (name, ports)
    if rdflib:
        graph = rdflib.Graph().parse(path, format='turtle')
        lv2 = rdflib.Namespace('http://lv2plug.in/ns/lv2core#')
        # Namespace inherits str: .index is the string method, not the RDF URI.
        nodes = list(graph.subjects(lv2['index'], None))
        assert len(nodes) == len(ports), (name, 'RDF port traversal', len(nodes))
        for node in nodes:
            if (node, rdflib.RDF.type, lv2.ControlPort) in graph:
                low, default, high = (float(graph.value(node, p)) for p in (lv2.minimum, lv2.default, lv2.maximum))
                assert low <= default <= high, (name, graph.value(node, lv2.symbol))
    print(f'PASS {name}: exact {len(ports)}-port enum/TTL contract; RDF {"parsed" if rdflib else "unavailable (install rdflib)"}')
