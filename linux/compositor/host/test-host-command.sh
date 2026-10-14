#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
output=${1:?usage: test-host-command.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
javac -d "$output" "$root"/app/src/main/java/org/matonos/compositor/{DBusSignature,DBusReader,DBusWriter,PortalWire,HostCommandBackend}.java \
    "$root/app/src/test/java/org/matonos/compositor/HostCommandBackendTest.java"
java -cp "$output" org.matonos.compositor.HostCommandBackendTest
