#!/usr/bin/env python3
"""Check that ttl/suprnam.ttl is valid RDF and describes the plugin we think it does.

test_nam_host.cpp reads the same file, but with a line-oriented regex scanner,
because parsing Turtle in C++ would mean taking a dependency on lilv just for
the tests. That scanner is good at catching a port index drifting away from the
PortIndex enum and completely blind to the file being malformed.

Which is not hypothetical. A stray double quote inside an rdfs:comment ended a
string literal early, orphaned every port declared after it, and left lilv
loading the plugin with 9 of its 45 ports and no latency port. Nothing errored:
the plugin loaded, the host test passed, and the damage was only visible in
lv2info. So the real parser runs here, and the regex scanner keeps its job of
checking the port contract.

Skips itself (exit 0, with a warning) when rdflib is not installed, so it never
blocks a build on a machine that does not have it.
"""

import sys

PLUGIN = "https://suprduprnatural.github.io/supr-pedals/nam"
EXPECTED_PORTS = 42
EXPECTED_FILE_PROPERTIES = 3

LV2 = "http://lv2plug.in/ns/lv2core#"
PIPEDAL_UI = "http://github.com/rerdavies/pipedal/ui#"


def main(path):
    try:
        import rdflib
    except ImportError:
        print("  SKIP  rdflib not installed; cannot check Turtle validity")
        print("        (pip install rdflib - worth it, this catches what the C++ test cannot)")
        return 0

    graph = rdflib.Graph()
    try:
        graph.parse(path, format="turtle")
    except Exception as exc:
        print("  FAIL  %s is not valid Turtle:\n        %s" % (path, exc))
        return 1

    lv2 = rdflib.Namespace(LV2)
    ui = rdflib.Namespace(PIPEDAL_UI)
    plugin = rdflib.URIRef(PLUGIN)
    failures = 0

    def check(ok, message):
        nonlocal failures
        print(("  PASS  " if ok else "  FAIL  ") + message)
        if not ok:
            failures += 1

    check(True, "%s parses as Turtle (%d triples)" % (path, len(graph)))

    # Ports have to be reachable *from the plugin*. The bug that prompted this
    # file left them syntactically fine but attached to nothing.
    ports = []
    for port in graph.objects(plugin, lv2["port"]):
        index = list(graph.objects(port, lv2["index"]))
        symbol = list(graph.objects(port, lv2["symbol"]))
        if not index or not symbol:
            check(False, "a port is missing lv2:index or lv2:symbol")
            continue
        ports.append((int(index[0]), str(symbol[0])))
    ports.sort()

    check(len(ports) == EXPECTED_PORTS,
          "plugin has %d ports (expected %d)" % (len(ports), EXPECTED_PORTS))

    indices = [i for i, _ in ports]
    check(indices == list(range(len(ports))),
          "port indices are contiguous from 0")

    symbols = [s for _, s in ports]
    check(len(symbols) == len(set(symbols)), "port symbols are unique")

    # The latency port is how the host compensates a parallel split against
    # threaded mode. Losing it is silent and audible only as a phase problem.
    latency = list(graph.subjects(lv2["designation"], lv2["latency"]))
    check(len(latency) == 1, "exactly one port carries lv2:designation lv2:latency")

    # Three model browsers, all pointed at the directory TooB NAM uses, which
    # is what makes the two plugins share a model library.
    files = []
    for node in graph.objects(plugin, ui["ui"]):
        files = list(graph.objects(node, ui["fileProperties"]))
    check(len(files) == EXPECTED_FILE_PROPERTIES,
          "%d file properties (expected %d)" % (len(files), EXPECTED_FILE_PROPERTIES))
    directories = {str(d) for f in files for d in graph.objects(f, ui["directory"])}
    check(directories == {"NeuralAmpModels"},
          "all model browsers point at NeuralAmpModels (found %s)"
          % (sorted(directories) or "none"))

    print("\n%s (%d failure%s)" % ("OK" if not failures else "FAILED",
                                   failures, "" if failures == 1 else "s"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "ttl/suprnam.ttl"))
