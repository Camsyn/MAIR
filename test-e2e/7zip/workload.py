#!/usr/bin/env python3
"""Create, test and extract a fresh archive, then compare its full contents."""

import pathlib
import random
import subprocess
import sys
import tempfile


def snapshot(root):
    paths = list(root.rglob("*"))
    for path in [root, *paths]:
        if path.is_symlink():
            raise RuntimeError(f"unexpected symbolic link: {path}")
    return {
        path.relative_to(root): path.read_bytes() if path.is_file() else None
        for path in paths
    }


def main():
    binary, root = map(pathlib.Path, sys.argv[1:])
    root.mkdir(parents=True, exist_ok=True)
    run = pathlib.Path(tempfile.mkdtemp(prefix="run-", dir=root))
    inputs = run / "input"
    (inputs / "nested" / "empty-dir").mkdir(parents=True)
    (inputs / "empty.txt").write_bytes(b"")
    (inputs / "text.txt").write_bytes(b"MAIR 7zip compression round trip\n" * 65536)
    (inputs / "nested" / "binary data.bin").write_bytes(
        random.Random(1).randbytes(2 * 1024 * 1024)
    )
    expected = {pathlib.Path("input"): None}
    expected.update(
        {pathlib.Path("input") / path: data for path, data in snapshot(inputs).items()}
    )
    archive = run / "payload.7z"
    output = run / "output"
    subprocess.run(
        [binary, "a", "-t7z", "-m0=lzma2", "-mx=1", "-mmt=2", archive, "input"],
        cwd=run,
        check=True,
    )
    subprocess.run([binary, "t", "-mmt=2", archive], check=True)
    subprocess.run([binary, "x", "-mmt=2", f"-o{output}", archive], check=True)
    if snapshot(output) != expected:
        raise RuntimeError(f"extracted files differ from inputs: {run}")
    print(f"7zip round trip ok: {run}")


if __name__ == "__main__":
    main()
