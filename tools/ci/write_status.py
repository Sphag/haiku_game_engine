#!/usr/bin/env python3
"""Write a machine-readable verification status record (US-004).

Statuses are explicit so an unavailable environment is never reported as passed:
  passed       the check ran and succeeded
  failed       the check ran and failed
  unsupported  the check cannot run yet because the feature or suite does not exist
  not-run      the environment or runner was not available
"""

import argparse
import json
import os
import sys

STATUSES = ("passed", "failed", "unsupported", "not-run")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="output JSON file")
    parser.add_argument("--environment", required=True, help="for example windows-dx12 or linux-vulkan")
    parser.add_argument("--status", required=True, choices=STATUSES)
    parser.add_argument("--reason", required=True, help="why the status is what it is")
    args = parser.parse_args()

    record = {
        "environment": args.environment,
        "status": args.status,
        "reason": args.reason,
        "commit": os.environ.get("GITHUB_SHA", ""),
    }

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(record, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"{args.environment}: {args.status} ({args.reason})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
