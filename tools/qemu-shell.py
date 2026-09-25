#!/usr/bin/env python3
"""qemu-shell.py: run a command in MatonOS's serial root shell under QEMU.

Usage: qemu-shell.py <serial socket> <command...>

run-qemu-live.sh exposes the serial console as <serial log>.sock; the debug
boot entry (androidboot.console=ttyS0) puts a root shell on it. The command's
output is printed between unique markers, so kernel log noise on the same
console is filtered out as far as possible.
"""
import socket
import sys
import time
import uuid

TIMEOUT = int(__import__("os").environ.get("QEMU_SHELL_TIMEOUT", "60"))


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path, cmd = sys.argv[1], " ".join(sys.argv[2:])
    tag = uuid.uuid4().hex[:8]
    begin, end = f"__BEGIN_{tag}__", f"__END_{tag}__"

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.settimeout(1.0)
    # Split the markers inside the command ("__BEGIN_""tag"), so the full
    # marker only ever appears in real output, never in the echoed command.
    half = len(begin) // 2
    b1, b2 = begin[:half], begin[half:]
    e1, e2 = end[:half], end[half:]
    line = (f'echo "{b1}""{b2}"; ( {cmd} ) 2>&1; echo "{e1}""{e2}"\n')
    s.sendall(b"\n" + line.encode())

    buf = b""
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            pass
        text = buf.decode(errors="replace")
        if end in text:
            break
    text = buf.decode(errors="replace").replace("\r", "")
    try:
        start = text.index(begin) + len(begin)
        stop = text.index(end, start)
    except ValueError:
        print(text[-4000:])
        print("qemu-shell: timed out or markers not found", file=sys.stderr)
        return 1
    out = [l for l in text[start:stop].splitlines()
           if not l.lstrip().startswith("[") or "audit" not in l]
    print("\n".join(out).strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
