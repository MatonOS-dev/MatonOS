#!/usr/bin/env python3
"""Dynamic ref database tests with an explicit-ref fake Flatpak uninstall."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='runtime-refs-') as temp:
    root = Path(temp)
    driver = root / 'driver'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source.parent / 'RuntimeRefs.c'),
                    str(source / 'runtime-refs-driver.c'), '-o', str(driver)], check=True)
    fake = root / 'flatpak'
    fake.write_text('''#!/usr/bin/env python3
import json, os, sys
with open(os.environ['REF_LOG'], 'a') as f: f.write(json.dumps(sys.argv[1:]) + '\\n')
sys.exit(1 if os.getenv('PRUNE_FAIL') else 0)
''')
    fake.chmod(0o755)
    log = root / 'log'
    env = dict(os.environ, FAKE_FLATPAK=str(fake), REF_LOG=str(log))
    runtime = 'runtime/org.example.Platform/x86_64/stable'
    extension = 'runtime/org.example.Platform.Locale/x86_64/stable'
    def run(*args, ok=True, extra=None):
        r = subprocess.run([str(driver), str(root), *map(str, args)], env=env | (extra or {}))
        assert (r.returncode == 0) == ok, args
    def calls():
        return [json.loads(x) for x in log.read_text().splitlines()] if log.exists() else []
    def db(): return json.loads((root / 'refs-0.json').read_text())
    for ref in (runtime, extension):
        for uid in (10120, 10122): run('add', ref, uid)
    assert (root / 'refs-0.json').stat().st_mode & 0o777 == 0o600
    run('remove', 10120)
    assert not calls() and db()[runtime] == [10122]
    run('remove', 10122)
    assert [c[-1] for c in calls()] == [extension, runtime] and db() == {}
    assert all('--system' in c and '--noninteractive' in c and '--unused' not in c for c in calls())
    print('PASS: shared runtime kept until last UID; extension pruned first')
    run('add', runtime, 10120)
    before = len(calls())
    run('reconcile')
    assert len(calls()) == before + 1 and db() == {}
    print('PASS: reconcile drops dead UID')
    run('add', runtime, 10122)
    run('remove', 10122, ok=False, extra={'PRUNE_FAIL': '1'})
    assert db() == {runtime: []}
    run('remove', 10122)
    assert db() == {}
    print('PASS: failed uninstall retained and retried')
    for malformed in ('oops', '{"runtime/org.example.Platform/x86_64/stable":[10120,]}',
                      '{"runtime/org.example.Platform/x86_64/stable":[],"runtime/org.example.Platform/x86_64/stable":[]}',
                      '{}\x00', '{"runtime/org.example.Platform/x86_64/stable":[10120,10120]}',
                      '{"runtime/org.example.Platform/x86_64/..":[]}', '{"runtime/org.example.Platform/x86_64/stable":[110120]}'):
        (root / 'refs-0.json').write_text(malformed)
        before = len(calls())
        run('remove', 10120, ok=False)
        assert len(calls()) == before
    print('PASS: corrupt, duplicate, foreign UID fail closed')
    database = root / 'refs-0.json'
    database.write_text('{}')
    database.chmod(0o644)
    before = len(calls())
    run('remove', 10120, ok=False)
    assert len(calls()) == before
    database.chmod(0o600)
    if os.geteuid() == 0:
        os.chown(database, 12345, -1)
        run('remove', 10120, ok=False)
        assert len(calls()) == before
        os.chown(database, 0, -1)
        print('PASS: wrong-owner database fails closed')
    print('PASS: unsafe database mode fails closed')
    (root / 'refs-0.json').unlink()
    before = len(calls())
    run('remove', 10120)
    assert len(calls()) == before and db() == {}
    print('PASS: missing database is empty and cleanup is allowed')
    newer = 'runtime/org.example.Platform/x86_64/new'
    run('add', runtime, 10120)
    run('add', newer, 10120)
    run('replace', newer, 10120)
    assert calls()[-1][-1] == runtime and db() == {newer: [10120]}
    run('add', runtime, 10120)
    run('add', runtime, 10122)
    before = len(calls())
    run('replace', newer, 10120)
    assert len(calls()) == before and db()[runtime] == [10122]
    print('PASS: exact update prunes old runtime only without another owner')

    # Exercise load, growth and save beyond all former database caps.
    owners = list(range(10000, 10300))
    large = {f"runtime/org.example.Platform{i}/x86_64/stable": owners[:] for i in range(2050)}
    database.write_text(json.dumps(large))
    assert database.stat().st_size > 1024 * 1024
    before = len(calls())
    run('add', runtime, 10300)
    run('add', next(iter(large)), 10300)
    state = db()
    assert len(state) == 2051 and len(state[next(iter(large))]) == 301
    assert state[runtime] == [10300] and not calls()[before:]
    print('PASS: >1 MiB database, >2048 refs and >256 owners load/grow/save')
    database.unlink()
    database.symlink_to(root / 'missing')
    run('remove', 10120, ok=False)
    assert len(calls()) == before
    print('PASS: symlink database fails closed')
    # Fail each allocator call in turn: no failed allocation may reach uninstall.
    database.unlink()
    wrapper = root / 'alloc-fail.c'
    wrapper.write_text('''#include <stdlib.h>
#include <stddef.h>
static size_t calls;
static int fail(void) {
    const char* at = getenv("ALLOC_FAIL_AT");
    return at && ++calls == strtoul(at, NULL, 10);
}
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __wrap_malloc(size_t n) { return fail() ? NULL : __real_malloc(n); }
void* __wrap_calloc(size_t n, size_t s) { return fail() ? NULL : __real_calloc(n, s); }
void* __wrap_realloc(void* p, size_t n) { return fail() ? NULL : __real_realloc(p, n); }
''')
    fault_driver = root / 'fault-driver'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source.parent / 'RuntimeRefs.c'),
                    str(source / 'runtime-refs-driver.c'), str(wrapper),
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc', '-o', str(fault_driver)], check=True)
    failures = 0
    for allocation in range(1, 30):
        database.write_text(json.dumps({runtime: [], extension: []}))
        database.chmod(0o600)
        before = len(calls())
        result = subprocess.run([str(fault_driver), str(root), 'remove', '10120'],
                                env=env | {'ALLOC_FAIL_AT': str(allocation)})
        if result.returncode:
            failures += 1
            assert len(calls()) == before
        else:
            assert db() == {} and len(calls()) == before + 2
            break
    else:
        raise AssertionError('allocator fault sweep never completed')
    assert failures >= 4
    print('PASS: allocation failures fail closed before any prune')
