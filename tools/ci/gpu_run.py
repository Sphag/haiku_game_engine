#!/usr/bin/env python3
"""Run the GPU suite on a self-hosted GPU runner and record its status (US-004).

engine_tests exits 3 when the requested backend cannot run here (for example, DX12 off Windows, or
Vulkan before US-028). That is recorded as `unsupported`, never `passed`. Any other nonzero exit,
including a usage error, is `failed`, and so is a missing binary, because the runner should have
built it. Only exit 0 is `passed`.
"""

import argparse
import json
import os
import subprocess
import sys

TESTS_EXE = "engine_tests.exe" if os.name == "nt" else "engine_tests"
UNSUPPORTED_EXIT = 3  # engine_tests: the requested GPU backend is not available in this build or on this host


def write_record(path, record):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(record, handle, indent=2, sort_keys=True)
        handle.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--environment", required=True)
    parser.add_argument("--backend", required=True, choices=("dx12", "vulkan"))
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--output", required=True, help="directory for the status record")
    args = parser.parse_args()

    record_path = os.path.join(args.output, f"gpu-{args.environment}.json")
    record = {
        "environment": args.environment,
        "backend": args.backend,
        "commit": os.environ.get("GITHUB_SHA", ""),
    }

    exe = os.path.join(args.build_dir, TESTS_EXE)
    if not os.path.isfile(exe):
        record.update(status="failed", reason=f"engine_tests not built at {exe}")
        write_record(record_path, record)
        print(record["reason"])
        return 1

    probe = subprocess.run([exe, "--suite", "gpu", "--backend", args.backend])
    if probe.returncode == UNSUPPORTED_EXIT:
        record.update(status="unsupported", reason=f"engine_tests reported the {args.backend} backend unavailable (exit {UNSUPPORTED_EXIT})")
    elif probe.returncode == 0:
        record.update(status="passed", reason="engine_tests --suite gpu passed")
    else:
        record.update(status="failed", reason=f"engine_tests --suite gpu exited {probe.returncode}")
    write_record(record_path, record)
    print(f"{args.environment}: {record['status']}")
    # unsupported is recorded, not failed: the environment lacks the backend, and the record says so.
    return 1 if record["status"] == "failed" else 0


if __name__ == "__main__":
    sys.exit(main())
