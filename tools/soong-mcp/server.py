#!/usr/bin/env python3
"""Local stdio MCP adapter for the MatonOS build coordinator (stdlib only)."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[5]
STATE = ROOT / 'out/pc-logs/soong-mcp'
IDENTITY = {'name': 'matonos-soong', 'version': '0.1.0'}
VERSIONS = ('2025-11-25', '2025-06-18', '2024-11-05')


def read(path):
    try:
        return path.read_text(errors='replace')
    except FileNotFoundError:
        return ''


def tail(path, lines=60):
    try:
        with path.open('rb') as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - 256 * 1024))
            return stream.read().decode(errors='replace').splitlines()[-lines:]
    except FileNotFoundError:
        return []


def save(data):
    STATE.mkdir(parents=True, exist_ok=True)
    temporary = STATE / 'state.tmp'
    temporary.write_text(json.dumps(data, indent=2))
    temporary.replace(STATE / 'state.json')


def state():
    return json.loads(read(STATE / 'state.json') or '{}')


def process_token(pid):
    try:
        # starttime distinguishes a managed process from a reused PID.
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]
    except (FileNotFoundError, ProcessLookupError):
        return None


def alive(data):
    return bool(data.get('pid') and process_token(data['pid']) == data.get('token'))


def build_lock():
    (ROOT / 'out').mkdir(exist_ok=True)
    handle = (ROOT / 'out/.maton-build.lock').open('a')
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        handle.close()
        raise ValueError('Another build owns the shared build lock.')
    return handle


def processes():
    found = []
    for item in Path('/proc').iterdir():
        if not item.name.isdigit():
            continue
        try:
            args = (item / 'cmdline').read_bytes().decode(errors='replace').split('\0')
            if args and Path(args[0]).name in {'siso', 'soong_build', 'soong_ui', 'ninja', 'ckati'}:
                found.append({'pid': int(item.name), 'command': args[:-1]})
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
    return found


LOGS = {
    'repair': ROOT / 'out/pc-logs/compositor-image-build.log',
    'coordinator': ROOT / 'out/pc-logs/test-build.log',
    'managed': STATE / 'build.log',
    'siso_errors': ROOT / 'out/siso_output',
}


def status():
    data = state()
    try:
        lock = build_lock()
        lock.close()
        locked = False
    except ValueError:
        locked = True
    logs = {}
    for name, path in LOGS.items():
        if path.exists():
            lines = tail(path, 12)
            progress = next((line for line in reversed(tail(path, 300))
                             if re.search(r'\[\d+/\d+\]|schedule pending:', line)), None)
            logs[name] = {'path': str(path), 'modified': path.stat().st_mtime,
                          'tail': lines, 'last_progress': progress}
    return {'build_lock_held': locked, 'processes': processes(),
            'managed_build': dict(data, process_alive=alive(data)), 'logs': logs,
            'repair_status': read(ROOT / 'out/pc-logs/agents/repair-image-status.txt'),
            'coordinator_status': tail(ROOT / 'out/pc-logs/agents/build-status.txt', 10)}


def artifacts():
    product = ROOT / 'out/target/product/pc_x86_64'
    image_directory = Path(os.environ.get('MATON_IMAGES_DIR', str(Path.home() / 'matonos-images'))).expanduser()
    paths = list(product.glob('*.img')) + list(image_directory.glob('*.img'))
    paths += list((ROOT / 'device/maton/pc_x86_64/prebuilt/apps-built').glob('*.apk'))
    return {'artifacts': [{'path': str(p), 'bytes': p.stat().st_size,
                           'modified': p.stat().st_mtime} for p in sorted(paths)],
            'note': 'Timestamps describe freshness, not proof that a fix is included or works.',
            'last_managed_build': state()}


def start(jobs=16, resume=False):
    if type(jobs) is not int or not 1 <= jobs <= 64:
        raise ValueError('jobs must be an integer between 1 and 64.')
    if alive(state()):
        raise ValueError('A managed build is already running.')
    if resume and not state():
        raise ValueError('No previous managed build to resume.')
    coordinator = ROOT / 'out/pc-logs/agents/coord-build.sh'
    if not coordinator.is_file():
        raise ValueError(f'Missing coordinator: {coordinator}')
    lock = build_lock()
    try:
        if processes():
            raise ValueError('Build processes are active outside the shared lock; wait for them.')
        STATE.mkdir(parents=True, exist_ok=True)
        log = (STATE / 'build.log').open('ab')
        try:
            child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                      '--root', str(ROOT),
                                      '--worker', str(jobs), '--lock-fd', str(lock.fileno())],
                                     cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                     start_new_session=True, pass_fds=(lock.fileno(),))
            data = {'pid': child.pid, 'token': process_token(child.pid),
                    'status': 'running', 'started': time.time(), 'jobs': jobs,
                    'log': str(STATE / 'build.log'),
                    'command': [str(coordinator), '--full', '-K', '-M', '-j', str(jobs)]}
            save(data)
        finally:
            log.close()
        return data
    finally:
        lock.close()


def stop():
    data = state()
    if not alive(data):
        raise ValueError('No live MCP-managed build. External builds must be stopped by their owner.')
    os.killpg(data['pid'], signal.SIGTERM)
    return {'status': 'stop_requested', 'pid': data['pid']}


def worker(jobs, lock_fd):
    # The inherited descriptor holds the same lock for all staging and packing.
    os.fstat(lock_fd)
    env = dict(os.environ, MATON_BUILD_LOCK_HELD='1', MATON_BUILD_COORDINATOR='1',
               MATON_BUILD_JOBS=str(jobs), MATON_CCACHE='0')
    command = [str(ROOT / 'out/pc-logs/agents/coord-build.sh'), '--full',
               '-K', '-M', '-j', str(jobs)]
    stopping = False
    def terminate(signum, frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, terminate)
    # Wait until the spawning server has recorded this worker's identity.
    for _ in range(100):
        if state().get('pid') == os.getpid():
            break
        time.sleep(.05)
    else:
        raise RuntimeError('Worker state was not recorded.')
    child = subprocess.Popen(command, cwd=ROOT, env=env, pass_fds=(lock_fd,))
    deadline = None
    while child.poll() is None:
        if stopping:
            deadline = deadline or time.monotonic() + 10
            child.terminate()
            if time.monotonic() >= deadline:
                data = state()
                data.update(status='stopped', finished=time.time())
                save(data)
                os.killpg(os.getpgrp(), signal.SIGKILL)
        time.sleep(.5)
    data = state()
    data.update(status='stopped' if stopping else ('success' if child.returncode == 0 else 'failed'),
                exit_code=child.returncode, finished=time.time())
    save(data)
    if stopping:
        # Coordinator exit does not imply all its compiler children exited.
        # Keep the inherited lock until the entire managed group is gone.
        os.killpg(os.getpgrp(), signal.SIGKILL)


def tool(name, description, properties=None, destructive=False):
    return {'name': name, 'description': description,
            'inputSchema': {'type': 'object', 'properties': properties or {},
                            'additionalProperties': False},
            'annotations': {'readOnlyHint': not destructive, 'destructiveHint': destructive}}


TOOLS = [
    tool('build_status', 'Read active build processes, lock, status files and recent progress. Scheduling is not completed compilation.'),
    tool('build_errors', 'Read bounded error output and failed commands. Error files may belong to an earlier build; compare timestamps.'),
    tool('build_log', 'Read the tail of a known build log.', {
        'log': {'type': 'string', 'enum': list(LOGS), 'default': 'repair'},
        'lines': {'type': 'integer', 'minimum': 1, 'maximum': 200, 'default': 60}}),
    tool('build_artifacts', 'List image and staged APK sizes and modification times.'),
    tool('build_start', 'Start the full coordinator with kernel/Mesa skipped, under the shared lock. Builds apps, partitions and live image. Does not publish. Refuses competing builds.',
         {'jobs': {'type': 'integer', 'minimum': 1, 'maximum': 64, 'default': 16}}, True),
    tool('build_stop', 'Stop only the MCP-managed build process group. Retains outputs for incremental resume.', destructive=True),
    tool('build_resume', 'Restart the last MCP-managed coordinator build, reusing existing outputs. No clean; job count can change.',
         {'jobs': {'type': 'integer', 'minimum': 1, 'maximum': 64, 'default': 16}}, True),
]


def call(name, args):
    schema = next((t['inputSchema'] for t in TOOLS if t['name'] == name), None)
    if schema is None or not isinstance(args, dict) or set(args) - set(schema['properties']):
        raise ValueError('Unknown tool or arguments.')
    if name == 'build_status':
        return status()
    if name == 'build_errors':
        paths = [ROOT / 'out/siso_output', ROOT / 'out/siso_failed_commands.sh']
        return {'files': [{'path': str(p), 'modified': p.stat().st_mtime,
                            'tail': tail(p, 120)} for p in paths if p.exists()]}
    if name == 'build_log':
        count = args.get('lines', 60)
        if type(count) is not int or not 1 <= count <= 200:
            raise ValueError('lines must be between 1 and 200.')
        key = args.get('log', 'repair')
        if key not in LOGS:
            raise ValueError('Unknown log.')
        return {'path': str(LOGS[key]), 'lines': tail(LOGS[key], count)}
    if name == 'build_artifacts':
        return artifacts()
    if name in ('build_start', 'build_resume'):
        return start(args.get('jobs', 16), name == 'build_resume')
    return stop()


def serve():
    for line in sys.stdin:
        request = None
        try:
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError('Expected a JSON-RPC object.')
            if 'id' not in request:
                continue
            method = request.get('method')
            params = request.get('params', {})
            if method == 'initialize':
                version = params.get('protocolVersion')
                result = {'protocolVersion': version if version in VERSIONS else VERSIONS[0],
                          'serverInfo': IDENTITY, 'capabilities': {'tools': {}}}
            elif method == 'ping':
                result = {}
            elif method == 'tools/list':
                result = {'tools': TOOLS}
            elif method == 'tools/call':
                try:
                    data = call(params.get('name'), params.get('arguments', {}))
                    result = {'content': [{'type': 'text', 'text': json.dumps(data)}]}
                except (ValueError, OSError) as error:
                    result = {'isError': True, 'content': [{'type': 'text', 'text': str(error)}]}
            else:
                print(json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                                  'error': {'code': -32601, 'message': 'Method not found'}}), flush=True)
                continue
            response = {'jsonrpc': '2.0', 'id': request['id'], 'result': result}
        except Exception as error:
            response = {'jsonrpc': '2.0', 'id': request.get('id') if isinstance(request, dict) else None,
                        'error': {'code': -32603, 'message': str(error)}}
        print(json.dumps(response), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--worker', type=int)
    parser.add_argument('--lock-fd', type=int)
    options = parser.parse_args()
    ROOT = options.root.resolve()
    STATE = ROOT / 'out/pc-logs/soong-mcp'
    LOGS.update(repair=ROOT / 'out/pc-logs/compositor-image-build.log',
                coordinator=ROOT / 'out/pc-logs/test-build.log',
                managed=STATE / 'build.log', siso_errors=ROOT / 'out/siso_output')
    if options.worker is not None:
        worker(options.worker, options.lock_fd)
    else:
        serve()
