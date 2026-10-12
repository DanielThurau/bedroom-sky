#!/usr/bin/env python3
"""Create a reviewed source-only copy, without local data or Git history."""
import argparse
import shutil
from pathlib import Path
from check_privacy import review


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New directory outside this checkout")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = args.output.resolve()
    if destination == root or root in destination.parents or destination.exists():
        parser.error("Output must be a new directory outside this checkout")
    files, findings = review(root)
    if findings:
        for name, label in findings:
            print(f"{name}: {label}")
        raise SystemExit("Export blocked by privacy findings; values withheld.")
    destination.mkdir(parents=True)
    for name in files:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, target)
    print(f"Exported {len(files)} reviewed files. No .git, private .env, caches, binaries, or images copied.")


if __name__ == "__main__":
    main()
