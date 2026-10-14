#!/usr/bin/env python3
"""Normalize linuxd's root/shell diagnostic snapshot for the test harness."""
import json
import sys

try:
    state = json.load(sys.stdin).get(sys.argv[1])
    if state is None:
        print('pending:0')
    else:
        if state.get('error'):
            raise ValueError(state['error'])
        phase = state['phase']
        if phase not in ('staging', 'publishing', 'handoff', 'complete'):
            raise ValueError('invalid operation phase')
        print(f"{phase}:{int(state['sequence'])}")
except (ValueError, KeyError, TypeError) as error:
    print(f'Flatpak operation failed: {error}', file=sys.stderr)
    sys.exit(1)
