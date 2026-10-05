#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: test-portals.sh OUTPUT_DIRECTORY}
mkdir -p "$OUT"
LIBS="$ROOT/app/libs/*"
javac -cp "$LIBS" -d "$OUT" "$ROOT/app/src/main/java/org/matonos/compositor/PortalWire.java" \
    "$ROOT/app/src/main/java/org/matonos/compositor/PortalBackend.java" \
    "$ROOT/app/src/main/java/org/matonos/compositor/PortalIntrospection.java" \
    "$ROOT/app/src/test/java/org/matonos/compositor/PortalTestPeer.java" \
    "$ROOT/app/src/test/java/org/matonos/compositor/PortalBackendTest.java"
java -cp "$OUT:$LIBS" org.matonos.compositor.PortalBackendTest
