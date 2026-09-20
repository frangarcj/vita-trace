#!/usr/bin/env python3
"""Record supplied capture artifacts and source provenance without modifying them.

Example: capture_manifest.py --artifact elf=build/game --artifact plugin=tracy_kernel.skprx
         --firmware 3.60 --output session.json

Hashes identify files, not the source revision that built them. Rebuild a matched
client/plugin pair, preserve the unstripped ELF, then record this manifest.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


def _stamp(info: os.stat_result) -> tuple[int, int, int, int]:
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def fingerprint(path: Path) -> dict:
    """Hash a regular file; reject replacements/modifications observed while reading."""
    path = path.expanduser().absolute()
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    fd = os.open(path, flags)
    with os.fdopen(fd, "rb") as handle:
        before = os.fstat(handle.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError(f"not a regular file: {path}")
        digest = hashlib.sha256()
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
        after = os.fstat(handle.fileno())
    if _stamp(before) != _stamp(after) or _stamp(before) != _stamp(path.stat(follow_symlinks=False)):
        raise ValueError(f"file changed while hashing: {path}")
    return {"path": str(path), "bytes": before.st_size, "sha256": digest.hexdigest()}


def _run(argv: list[str]) -> bytes:
    result = subprocess.run(argv, capture_output=True, check=True, timeout=20)
    return result.stdout


def git_snapshot(root: Path) -> dict:
    """A source snapshot, deliberately not a claim about the artifacts' build origin."""
    root = root.resolve()
    prefix = ["git", "-C", str(root)]
    try:
        top = Path(os.fsdecode(_run(prefix + ["rev-parse", "--show-toplevel"]).strip())).resolve()
        if top != root:
            raise ValueError("not a repository root (possibly an uninitialized submodule)")
        revision = _run(prefix + ["rev-parse", "HEAD"]).decode("ascii").strip()
        status = _run(prefix + ["status", "--porcelain=v1", "--untracked-files=all"]).decode("utf-8", "replace")
        patch = _run(prefix + ["diff", "--no-ext-diff", "--no-textconv", "--binary", "HEAD", "--", "."])
        return {"available": True, "root": str(root), "revision": revision,
                "dirty": bool(status), "status": status,
                "tracked_diff_sha256": hashlib.sha256(patch).hexdigest()}
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {"available": False, "root": str(root), "error": str(error)}


def compiler_snapshot(compiler: str) -> dict:
    try:
        version = _run([compiler, "--version"]).decode("utf-8", "replace").strip()
        if not version:
            raise ValueError("compiler returned no version")
        return {"available": True, "command": compiler, "version": version}
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {"available": False, "command": compiler, "error": str(error)}


def parse_artifacts(specs: list[str]) -> dict[str, Path]:
    artifacts: dict[str, Path] = {}
    for spec in specs:
        name, separator, path = spec.partition("=")
        if not separator or not re.fullmatch(r"[a-z][a-z0-9_-]*", name) or not path:
            raise ValueError(f"expected name=path with a lowercase artifact name: {spec}")
        if name in artifacts:
            raise ValueError(f"duplicate artifact name: {name}")
        artifacts[name] = Path(path)
    return artifacts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", action="append", required=True, metavar="NAME=PATH")
    parser.add_argument("--output", type=Path, required=True, help="new JSON file; never overwritten")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--compiler", default="arm-vita-eabi-gcc")
    parser.add_argument("--firmware", help="observed console firmware, not inferred from the build")
    parser.add_argument("--note", action="append", default=[], help="clocks, workload, rate, or test result")
    args = parser.parse_args(argv)
    try:
        artifacts = {name: fingerprint(path) for name, path in parse_artifacts(args.artifact).items()}
        root = args.source_root.resolve()
        manifest = {
            "schema_version": 1,
            "created_at_utc": datetime.now(timezone.utc).isoformat(),
            "firmware_reported": args.firmware,
            "notes": args.note,
            "artifacts": artifacts,
            "source": git_snapshot(root),
            "tracy_source": git_snapshot(root / "third_party" / "tracy"),
            "compiler": compiler_snapshot(args.compiler),
            "provenance_limit": "Source/compiler are snapshots at manifest creation, not proof that they built these files. "
                                "Diff hashes do not preserve local patches or untracked contents; save those separately.",
        }
        text = json.dumps(manifest, indent=2, ensure_ascii=True) + "\n"
        # Exclusive creation preserves an earlier run, even when names are reused.
        with args.output.open("x", encoding="utf-8") as handle:
            handle.write(text)
    except (OSError, ValueError) as error:
        print(f"capture_manifest: {error}", file=sys.stderr)
        return 1
    print(f"Recorded {len(artifacts)} artifacts in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
