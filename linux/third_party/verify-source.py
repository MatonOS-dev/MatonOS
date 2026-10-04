#!/usr/bin/env python3
"""Verify an exact release/fork source tree before a dependency build."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def tree_digest(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part in {'.git', '__pycache__'} for part in relative.parts) or path.is_dir():
            continue
        if path.is_symlink():
            import os
            content = b'link:' + os.readlink(path).encode()
        else:
            content = path.read_bytes()
        digest.update(relative.as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(content).digest())
    return digest.hexdigest()


if __name__ == '__main__':
    component, directory = sys.argv[1:]
    root = Path(directory).resolve()
    pin = json.loads((Path(__file__).parent / 'source-pins.json').read_text())[component]
    revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != pin['upstream_commit'] or tree_digest(root) != pin['source_tree_sha256']:
        sys.exit(f'{component}: source does not match the reviewed release/fork pin')
    print(f'{component}: verified {pin["version"]} source tree')
