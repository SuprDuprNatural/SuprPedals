"""Convert PiPedal's saved Mix/Effect Level controls to Dry/Wet percentages.

Run with PiPedal stopped. Originals are backed up before any atomic replacement.
Other controls, paths, bank entries and snapshot identities remain unchanged.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil

DEFAULT_MIX = {'echo': .2, 'space': .18, 'phase': .5, 'crush': .5, 'vowel': .8}
URIS = {f'https://suprduprnatural.github.io/supr-pedals/{name}': mix
        for name, mix in DEFAULT_MIX.items()}


def percentage_db(percent):
    assert math.isfinite(float(percent))
    percent = min(400, max(0, float(percent)))
    return max(-60, 20 * math.log10(percent / 50)) if percent else -60


def convert(document, decibels=False):
    def control_map(controls):
        return controls if isinstance(controls, dict) else {c['key']: c['value'] for c in controls}

    def binding_levels(binding, mix):
        symbol = binding.get('symbol')
        if symbol in ('mix', 'level'):
            binding['symbol'] = {'mix': 'dry', 'level': 'wet'}[symbol]
            for key in ('minValue', 'maxValue'):
                if key in binding:
                    amount = binding[key]
                    binding[key] = 100 * (1 - amount) if symbol == 'mix' else 100 * mix * 10 ** (amount / 20)
            if symbol == 'mix' and 'stepValue' in binding:
                binding['stepValue'] *= -100
        if decibels and binding.get('symbol') in ('dry', 'wet'):
            for key in ('minValue', 'maxValue'):
                if key in binding:
                    binding[key] = percentage_db(binding[key])

    def walk(value, uri=None, instances=None):
        if isinstance(value, list):
            for child in value:
                walk(child, uri, instances)
            return
        if not isinstance(value, dict):
            return
        if 'items' in value:
            instances = {}
            mixes = {}
            def collect(items):
                for item in items:
                    instances[item['instanceId']] = item.get('uri')
                    if item.get('uri') in URIS:
                        mixes[item['instanceId']] = control_map(item.get('controlValues', [])).get('mix', URIS[item['uri']])
                    collect(item.get('topChain', []))
                    collect(item.get('bottomChain', []))
            collect(value['items'])
            for binding in value.get('gpioBindings', []):
                if instances.get(binding.get('instanceId')) in URIS:
                    binding_levels(binding, mixes[binding['instanceId']])
            if instances.get(value.get('gpioScrollInstanceId')) in URIS:
                symbol = value.get('gpioScrollSymbol')
                if symbol in ('mix', 'level'):
                    value['gpioScrollSymbol'] = {'mix': 'dry', 'level': 'wet'}[symbol]
        uri = value.get('uri', value.get('pluginUri', (instances or {}).get(value.get('instanceId'), uri)))
        controls = value.get('controlValues')
        if uri in URIS and isinstance(controls, (list, dict)):
            old = control_map(controls)
            legacy = 'mix' in old or 'level' in old
            if legacy:
                mix = float(old.get('mix', URIS[uri]))
                level = float(old.get('level', 0))
                assert math.isfinite(mix) and 0 <= mix <= 1
                assert math.isfinite(level) and -24 <= level <= 12
                levels = {'dry': 100 * (1 - mix), 'wet': 100 * mix * 10 ** (level / 20)}
                if isinstance(controls, dict):
                    controls.pop('mix', None)
                    controls.pop('level', None)
                    controls.update(levels)
                else:
                    for control in controls:
                        key = control['key']
                        if key in ('mix', 'level'):
                            control['key'] = {'mix': 'dry', 'level': 'wet'}[key]
                            control['value'] = levels[control['key']]
                    for key, amount in levels.items():
                        if not any(c['key'] == key for c in controls):
                            controls.append({'key': key, 'value': amount})
                for binding in value.get('midiBindings', []):
                    binding_levels(binding, mix)
            if decibels:
                if isinstance(controls, dict):
                    for key in ('dry', 'wet'):
                        if key in controls:
                            controls[key] = percentage_db(controls[key])
                else:
                    for control in controls:
                        if control['key'] in ('dry', 'wet'):
                            control['value'] = percentage_db(control['value'])
                if not legacy:
                    for binding in value.get('midiBindings', []):
                        binding_levels(binding, URIS[uri])
        for child in value.values():
            walk(child, uri, instances)
    walk(document)
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data_root', type=Path)
    parser.add_argument('backup', type=Path)
    parser.add_argument('--db', action='store_true', help='Convert installed percentage return levels to dB (0 dB = 50%).')
    args = parser.parse_args()
    if args.db:
        # Refuse a second conversion once the installed controls use dB.
        for effect in DEFAULT_MIX:
            ttl = Path(f'/usr/local/lib/lv2/supr{effect}.lv2/supr{effect}.ttl').read_text()
            assert any('lv2:symbol "dry"' in line and 'units:unit units:pc' in line
                       for line in ttl.splitlines()), f'{effect}: expected percentage metadata before conversion'
    files = list((args.data_root / 'presets').glob('*.bank'))
    files += list((args.data_root / 'plugin_presets').glob('*.json'))
    current = args.data_root / 'currentPreset.json'
    if current.exists():
        files.append(current)
    changes = []
    for path in files:
        text = path.read_text()
        original = json.loads(text)
        updated = convert(json.loads(text), args.db)
        if updated != original:
            changes.append((path, json.dumps(updated, ensure_ascii=False, indent=2) + '\n'))
    # Parse every source before changing any file; retain its ownership and mode.
    for path, text in changes:
        saved = args.backup / path.relative_to(args.data_root)
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, saved)
    for path, text in changes:
        staged = path.with_name(path.name + '.dry-wet-new')
        shutil.copy2(path, staged)
        staged.write_text(text)
        stat = path.stat()
        os.chown(staged, stat.st_uid, stat.st_gid)
        os.replace(staged, path)
    print(f'Converted {len(changes)} saved preset files; originals in {args.backup}')


if __name__ == '__main__':
    main()
