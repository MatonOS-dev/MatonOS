#!/usr/bin/env python3
"""Shared payload preparation and compatibility metadata for vendor APEXes."""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def elf(path):
    try:
        with path.open('rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


def prepare(args):
    payload = pathlib.Path(args.payload)
    product = pathlib.Path(args.product_out)
    temp = pathlib.Path(args.temp)
    for root in map(pathlib.Path, args.lib_root):
        changed = True
        while changed:
            changed = False
            for binary in (p for p in payload.rglob('*') if p.is_file() and elf(p)):
                dynamic = run(args.readelf, '-d', str(binary))
                for name in re.findall(r'Shared library: \[(.*?)\]', dynamic):
                    if any((payload / d / name).is_file() for d in ('lib64', 'lib64/soundfx')):
                        continue
                    candidate = root / name
                    if candidate.is_file():
                        target = payload / 'lib64' / name
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(candidate, target)
                        print(f'vendor-apex: bundling library {candidate}', file=sys.stderr)
                        changed = True
    needed = set()
    for binary in (p for p in payload.rglob('*') if p.is_file() and elf(p)):
        for lib in re.findall(r'Shared library: \[(.*?)\]', run(args.readelf, '-d', str(binary))):
            if not any((payload / d / lib).is_file() for d in ('lib64', 'lib64/soundfx')):
                needed.add(lib)
    for path in args.xml:
        for element in ET.parse(path).iter('library'):
            needed.add(element.attrib['path'])
    for lib in args.require_needed:
        if lib not in {n for p in payload.rglob('*') if p.is_file() and elf(p)
                       for n in re.findall(r'Shared library: \[(.*?)\]', run(args.readelf, '-d', str(p)))}:
            raise SystemExit(f'required DT_NEEDED entry missing: {lib}')
    for rel in sorted(payload.rglob('* *'), key=lambda p: len(p.parts), reverse=True):
        name = rel.relative_to(payload).as_posix()
        if not name.startswith('etc/alsa/ucm2/'):
            raise SystemExit(f'payload path contains a space, apexer cannot package it: {name}')
        print(f'vendor-apex: dropping UCM profile with a space in its path: {name}', file=sys.stderr)
        rel.unlink()
    manifest = {'name': args.name, 'version': int(args.version), 'requireNativeLibs': sorted(needed)}
    (temp / 'apex_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    with (temp / 'canned_fs_config').open('w') as f:
        f.write('/ 0 0 0755\n/apex_manifest.pb 0 0 0644\n/apex_manifest.json 0 0 0644\n')
        for p in sorted(payload.rglob('*')):
            rel = '/' + p.relative_to(payload).as_posix()
            mode = 0o755 if p.is_dir() or p.stat().st_mode & 0o111 else 0o644
            f.write(f'{rel} 0 0 {mode:04o}\n')
    android = (f'<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
               f'package="{args.name}" android:versionCode="{args.version}" '
               f'android:versionName="{args.version}"><application android:hasCode="false" '
               '/></manifest>\n')
    (temp / 'AndroidManifest.xml').write_text(android)


def compat(args):
    payload = pathlib.Path(args.payload)
    product = pathlib.Path(args.product_out)
    elfs = []
    for path in payload.rglob('*'):
        if elf(path):
            dynamic = run(args.readelf, '-d', str(path))
            deps = sorted(set(re.findall(r'Shared library: \[(.*?)\]', dynamic)))
            soname = re.search(r'SONAME.*\[(.*?)\]', dynamic)
            elfs.append((path, deps, soname.group(1) if soname else None))
    internal = {p.name for p, _, _ in elfs if p.relative_to(payload).parts[0] == 'lib64'}
    internal.update(soname for p, _, soname in elfs
                    if soname and p.relative_to(payload).parts[0] == 'lib64')
    external = sorted({lib for _, deps, _ in elfs for lib in deps if lib not in internal})
    root = ET.parse(args.vintf).getroot()
    hals = []
    for hal in root.findall('hal'):
        hals.append({
            'format': hal.get('format'), 'name': hal.findtext('name'),
            'versions': sorted(x.text for x in hal.findall('version') if x.text),
            'interfaces': [{'name': i.findtext('name'), 'instances': sorted(
                x.text for x in i.findall('instance') if x.text)} for i in hal.findall('interface')],
            'updatable_via_apex': hal.get('updatable-via-apex'),
        })
    props = {}
    for rel in ('system/build.prop', 'vendor/build.prop', 'product/build.prop', 'odm/etc/build.prop'):
        file = product / rel
        if file.is_file():
            for line in file.read_text(errors='replace').splitlines():
                if '=' in line and not line.lstrip().startswith('#'):
                    k, v = line.split('=', 1)
                    props.setdefault(k, v)
    try:
        sdk, update = int(props['ro.build.version.sdk']), int(props['ro.build.date.utc'])
    except (KeyError, ValueError) as exc:
        raise SystemExit(f'cannot read positive SDK/build date from {product}: {exc}') from exc
    if sdk <= 0 or update <= 0:
        raise SystemExit('ro.build.version.sdk and ro.build.date.utc must be positive')
    result = {'schema_version': 1, 'name': args.name, 'apex_version': int(args.version),
              'needed_shared_libraries': external, 'vintf': {'hals': hals},
              'base_android': sdk, 'update_id': str(update)}
    pathlib.Path(args.output).write_text(json.dumps(result, indent=2) + '\n')


p = argparse.ArgumentParser()
sub = p.add_subparsers(dest='command', required=True)
prep = sub.add_parser('prepare')
for name in ('payload', 'product_out', 'temp', 'name', 'version', 'readelf'):
    prep.add_argument('--' + name.replace('_', '-'), required=True)
prep.add_argument('--lib-root', action='append', default=[])
prep.add_argument('--xml', action='append', default=[])
prep.add_argument('--require-needed', action='append', default=[])
prep.set_defaults(func=prepare)
record = sub.add_parser('compat')
for name in ('payload', 'product_out', 'vintf', 'name', 'version', 'readelf', 'output'):
    record.add_argument('--' + name.replace('_', '-'), required=True)
record.set_defaults(func=compat)
a = p.parse_args()
a.func(a)
