#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/maton-repo-java-XXXXXX)
trap 'rm -rf "$work"' EXIT
javac -d "$work" "$root/systembridge/src/org/matonos/systembridge/FlatpakRepo.java" \
  "$root/systembridge/src/org/matonos/systembridge/LinuxAppStorePolicy.java" \
  "$root/systembridge/src/org/matonos/systembridge/RepoDecision.java" \
  "$root/systembridge/tests/repo/RepoManagementTest.java"
java -cp "$work" org.matonos.systembridge.RepoManagementTest
