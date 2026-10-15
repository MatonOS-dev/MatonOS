#!/usr/bin/env python3
"""Real fs-verity retained-writer regression against linuxd's seal primitive.

Use MATON_VERITY_TEST_DIR on a verity-enabled filesystem. An unsupported
filesystem is a SKIP (77), never a successful mapping regression. Pass
--require-verity to make that condition a failure. No mount, Android install,
or SELinux labels are changed. There is no private-copy alternative.
"""
import argparse
import errno
import mmap
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-verity", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="maton-staged-handoff-",
                                     dir=os.environ.get("MATON_VERITY_TEST_DIR")) as tmp:
        root = Path(tmp)
        driver = root / "verity-driver"
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-DMATONOS_PUBLISH_HOST_TEST", "-ffunction-sections",
                        "-fdata-sections", "-Wl,--gc-sections",
                        str(Path(__file__).with_name("staged-verity-driver.c")),
                        str(Path(__file__).parents[1] / "FlatpakPublish.c"),
                        "-o", str(driver)], check=True)

        def seal(name, parent=root):
            result = subprocess.run([str(driver), str(parent), name], check=True,
                                    text=True, capture_output=True)
            code, _, message = result.stdout.partition("\n")
            return int(code), message.strip()

        # Detect unsupported filesystems using an inode with no writers. Do
        # not misreport EOPNOTSUPP as proof of retained-writer protection.
        control = root / "control"
        control.write_bytes(b"sealed code\n")
        code, message = seal("control")
        if code in (errno.EOPNOTSUPP, errno.ENOTTY):
            assert message == "Cannot trust /data, expected fs feature 'fs-verity' isn't available"
            print("SKIP: real verity mapping regression: filesystem unsupported")
            return 1 if args.require_verity else 77
        assert code == 0, (code, message)
        try:
            control.open("r+b").close()
        except OSError as exc:
            assert exc.errno == errno.EPERM, exc
        else:
            raise AssertionError("sealed control reopened for writing")

        staging = root / "staging"
        staging.mkdir()
        source = staging / "payload"
        source.write_bytes(b"verified code\n")
        # Exercise a writer fd independently, then a mapping whose original
        # writable fd has been closed. Both must make sealing fail ETXTBSY.
        with source.open("r+b"):
            code, message = seal("payload", staging)
            assert code == errno.ETXTBSY, (code, message)
            assert message == "staged file still open for writing"
        with source.open("r+b") as writer:
            mapping = mmap.mmap(writer.fileno(), 0, access=mmap.ACCESS_WRITE)
        try:
            os.chmod(source, 0o444)
            code, message = seal("payload", staging)
            assert code == errno.ETXTBSY, (code, message)
            assert message == "staged file still open for writing"
            # A failed immutable-input boundary must never reach rename.
            published = root / "published"
            if code == 0:
                staging.rename(published)
            assert not published.exists()
            mapping[:] = b"modified code\n"
            mapping.flush()
        finally:
            mapping.close()
        assert seal("payload", staging)[0] == 0
        print("PASS: retained writer fd and mapping reject sealing; no publication")
        print("PASS: seal succeeds after writer release; sealed control rejects writes")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
