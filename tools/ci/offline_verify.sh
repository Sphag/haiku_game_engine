#!/usr/bin/env bash
# Offline build verification (US-004). Runs configure, build, and unit tests with no network,
# starting from an empty build directory. A cache miss must not trigger a download, so any
# attempted download fails the job.
#
# Usage: bash tools/ci/offline_verify.sh [preset-suffix]   (default: linux-dev)
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
preset="${1:-linux-dev}"

# Re-run inside a fresh user and network namespace that has only the loopback interface.
if [ "${ENGINE_OFFLINE_NAMESPACE:-}" != "1" ]; then
  if ! command -v unshare >/dev/null 2>&1; then
    echo "offline_verify: unshare (util-linux) is required to disable networking" >&2
    exit 1
  fi
  exec env ENGINE_OFFLINE_NAMESPACE=1 unshare -rn bash "$0" "$@"
fi

# Prove networking is disabled: only lo may exist, and a TCP connection must fail.
interfaces="$(awk -F: 'NR > 2 { gsub(/ /, "", $1); print $1 }' /proc/net/dev)"
if [ "$interfaces" != "lo" ]; then
  echo "offline_verify: network interfaces other than lo are present: $interfaces" >&2
  exit 1
fi
if python3 -I -c 'import socket; socket.create_connection(("1.1.1.1", 443), timeout=3)' 2>/dev/null; then
  echo "offline_verify: outbound TCP succeeded; networking is not disabled" >&2
  exit 1
fi
echo "offline_verify: network disabled (interfaces: $interfaces)"

cd "$repo"
build_dir="build/$preset"
rm -rf "$build_dir"
if [ -e "$build_dir" ]; then
  echo "offline_verify: $build_dir could not be removed" >&2
  exit 1
fi

cmake -P cmake/ValidateVendor.cmake
cmake --preset "$preset"
cmake --build --preset "${preset%-dev}-debug"
mkdir -p artifacts/reports
ctest --preset "${preset%-dev}-unit" --output-on-failure --no-tests=error \
  --output-junit "$repo/artifacts/reports/offline-$preset-unit.xml"
echo "offline_verify: passed with networking disabled"
