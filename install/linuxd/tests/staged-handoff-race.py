#!/usr/bin/env python3
"""Demonstrate why staged code needs a private/immutable verification input.

This is a threat-model probe, not an installer implementation test. It uses
only a temporary directory and the current UID. chmod/rename cannot revoke a
MAP_SHARED writer; chown likewise does not invalidate an existing mapping.
No Android installation or SELinux label is changed.
"""
import hashlib
import mmap
import os
from pathlib import Path
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def main():
    with tempfile.TemporaryDirectory(prefix="maton-staged-handoff-") as tmp:
        root = Path(tmp)
        staging = root / "staging"
        staging.mkdir()
        source = staging / "payload"
        original = b"verified code\n"
        replaced = b"modified code\n"
        assert len(original) == len(replaced)
        source.write_bytes(original)
        with source.open("r+b") as writer:
            with mmap.mmap(writer.fileno(), 0, access=mmap.ACCESS_WRITE) as mapping:
                expected = digest(source)
                # A producer can keep this mapping after reporting completion.
                os.chmod(source, 0o444)
                published = root / "published"
                staging.rename(published)
                target = published / "payload"
                assert digest(target) == expected
                mapping[:] = replaced
                mapping.flush()
                assert digest(target) != expected
                assert target.stat().st_mode & 0o777 == 0o444
                print("CONFIRMED: writable mapping changes verified renamed file")

                # A private inode breaks the producer's alias. Verification
                # must happen on this copy, not before copying the source.
                private = root / "private"
                private.write_bytes(target.read_bytes())
                private_expected = digest(private)
                mapping[:] = original
                mapping.flush()
                assert digest(private) == private_expected
                assert digest(target) != private_expected
                assert private.stat().st_ino != target.stat().st_ino
                print("PASS: private inode remains unchanged by producer mapping")


if __name__ == "__main__":
    main()
