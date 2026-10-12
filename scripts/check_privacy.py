#!/usr/bin/env python3
"""Review publishable source, staged files, or reachable Git history without printing secrets."""
import argparse
import re
import subprocess
from pathlib import Path

ROOT_FILES = {".gitignore", ".env.example", "CMakeLists.txt", "README.md", "CLAUDE.md", "SPEC.md"}
SOURCE_DIRS = {"src", "include", "scripts", "tests", "docs"}
EXTENSIONS = {".c", ".cpp", ".h", ".hpp", ".py", ".md", ".txt"}
PATTERNS = {
    "private key": rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----",
    "GitHub token": rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})\b",
    "AWS access key": rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b",
    "Google API key": rb"\bAIza[A-Za-z0-9_-]{35}\b",
    "Slack token": rb"\bxox[baprs]-[A-Za-z0-9-]{20,}\b",
    "credential assignment": rb"(?i)(?:api[_-]?key|secret|password|access[_-]?token)\s*[:=]\s*[\"'][^\"'\r\n]{8,}[\"']",
    "personal filesystem path": rb"(?:/Users/|/home/)[A-Za-z0-9_.-]+/|[A-Za-z]:\\Users\\",
}
EMAIL = re.compile(rb"[A-Za-z0-9_.+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")


def allowed(name):
    path = Path(name)
    if path.name.endswith(".local.md"):
        return False
    return name in ROOT_FILES or (
        len(path.parts) > 1 and path.parts[0] in SOURCE_DIRS
        and all(not part.startswith(".") and part != "__pycache__" for part in path.parts)
        and path.suffix in EXTENSIONS
    )


def source_files(root):
    files = [name for name in ROOT_FILES if (root / name).is_file()]
    for directory in SOURCE_DIRS:
        for path in (root / directory).rglob("*"):
            name = path.relative_to(root).as_posix()
            if path.is_file() and allowed(name):
                if path.is_symlink():
                    raise ValueError("Source symlinks require manual review")
                files.append(name)
    for name in files:
        path = root / name
        while path != root:
            if path.is_symlink():
                raise ValueError("Source symlinks require manual review")
            path = path.parent
    return sorted(files)


def private_coordinate_markers(root):
    # Derive markers locally; never embed a person's location in this script/report.
    def read(path):
        result = {}
        if path.is_file():
            for line in path.read_text().splitlines():
                key, equal, value = line.partition("=")
                if equal:
                    result[key.strip()] = value.split("#")[0].strip().strip("\"'")
        return result
    private, demo = read(root / ".env"), read(root / ".env.example")
    markers = set()
    for key in ("BEDROOM_SKY_LATITUDE", "BEDROOM_SKY_LONGITUDE"):
        if key in private and private[key] != demo.get(key):
            try:
                value = abs(float(private[key]))
                # Include approximate location, as older settings may vary slightly.
                markers.add(f"{value:.4f}".encode())
                markers.add(f"{value:.4f}"[:-1].encode())
            except ValueError:
                pass
    return markers


def scan_bytes(data, markers=()):
    findings = [label for label, pattern in PATTERNS.items() if re.search(pattern, data)]
    if any(not email.endswith(b"@users.noreply.github.com") and not email.endswith(b"@example.com")
           for email in EMAIL.findall(data)):
        findings.append("email address needs review")
    if any(marker in data for marker in markers):
        findings.append("local observer location")
    return findings


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def review(root, staged=False, history=False):
    markers = private_coordinate_markers(root)
    findings = []
    files = source_files(root)
    for name in files:
        findings.extend((name, label) for label in scan_bytes((root / name).read_bytes(), markers))
    if staged:
        for raw in git(root, "ls-files", "-z").split(b"\0"):
            if not raw:
                continue
            name = raw.decode()
            if not allowed(name):
                findings.append((name, "file is outside the reviewed publish list"))
            findings.extend((name, label) for label in scan_bytes(git(root, "show", ":" + name), markers))
    if history:
        objects = git(root, "rev-list", "--objects", "--all").decode().splitlines()
        process = subprocess.Popen(["git", "-C", str(root), "cat-file", "--batch"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        for entry in objects:
            sha, _, name = entry.partition(" ")
            process.stdin.write((sha + "\n").encode())
            process.stdin.flush()
            info = process.stdout.readline().split()
            size = int(info[2])
            data = process.stdout.read(size)
            process.stdout.read(1)
            if info[1] in (b"blob", b"commit", b"tag"):
                findings.extend(("history:" + (name or sha[:12]), label)
                                for label in scan_bytes(data, markers))
            if info[1] == b"blob" and name and not allowed(name):
                findings.append(("history:" + name, "non-source artifact in history"))
        process.stdin.close()
        process.wait()
        process.stdout.close()
    return files, sorted(set(findings))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--staged", action="store_true", help="Also inspect the Git index")
    parser.add_argument("--history", action="store_true", help="Also inspect every reachable Git object")
    args = parser.parse_args()
    try:
        files, findings = review(args.root, args.staged, args.history)
    except (ValueError, OSError, subprocess.CalledProcessError):
        print("Privacy review failed to read inputs; details withheld.")
        return 2
    for name, label in findings:
        print(f"{name}: {label}")
    print(f"Reviewed {len(files)} source files; {len(findings)} findings. Values are never printed.")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
