#!/usr/bin/env python3
"""Add a hardware test build to the test-builds branch (zips and an index
only, so main stays small) and push it.

usage: publish_testbuild.py ZIP N "what's new" [--repo DIR] [--commit SHA]

The build is linked to SHA (default: the repo's HEAD). Run from a clone
whose 'origin' is the GitHub repository.
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def git(*a, cwd):
    return subprocess.run(["git", *a], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip", type=Path)
    ap.add_argument("n", type=int)
    ap.add_argument("new")
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--commit")
    a = ap.parse_args()
    sha = git("rev-parse", a.commit or "HEAD", cwd=a.repo)
    url = git("remote", "get-url", "origin", cwd=a.repo).removesuffix(".git")
    with tempfile.TemporaryDirectory() as td:
        wt = Path(td) / "tb"
        git("fetch", "-q", "origin", "test-builds", cwd=a.repo)
        git("worktree", "add", "-q", "--detach", str(wt), "FETCH_HEAD", cwd=a.repo)
        try:
            name = Path(a.zip).name   # KernelExEx-1.0.0-testbuildN.zip from build 9 on
            shutil.copy(a.zip, wt / name)
            idx = wt / "README.md"
            lines = idx.read_text().splitlines()
            row = f"| {a.n} | [{name}]({name}) | [{sha[:7]}]({url}/commit/{sha}) | {a.new} |"
            lines = [l for l in lines if not l.startswith(f"| {a.n} |")]
            at = next(i for i, l in enumerate(lines) if l.startswith("|---")) + 1
            lines.insert(at, row)
            idx.write_text("\n".join(lines) + "\n")
            git("add", "-A", cwd=wt)
            git("commit", "-qm", f"Hardware test build {a.n}\n\n{a.new}", cwd=wt)
            git("push", "-q", "origin", "HEAD:test-builds", cwd=wt)
            print(f"{name}: {url}/blob/test-builds/{name}")
        finally:
            git("worktree", "remove", "--force", str(wt), cwd=a.repo)


if __name__ == "__main__":
    main()
