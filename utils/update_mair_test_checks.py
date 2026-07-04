#!/usr/bin/env python3

import os
import shutil
import subprocess
import sys
from pathlib import Path


def has_arg(args, name: str) -> bool:
    return any(arg == name or arg.startswith(name + "=") for arg in args)


def resolve_mair_opt_binary() -> str | None:
    if shutil.which("mair-opt"):
        return None

    mair_opt = os.environ.get("MAIR_OPT")
    if not mair_opt:
        print(
            "error: mair-opt not found. Add mair-opt to PATH or set MAIR_OPT "
            "to the mair-opt binary.",
            file=sys.stderr,
        )
        return ""

    mair_opt = os.path.abspath(os.path.expanduser(mair_opt))
    if not os.path.isfile(mair_opt) or not os.access(mair_opt, os.X_OK):
        print(
            f"error: MAIR_OPT does not name an executable file: {mair_opt}",
            file=sys.stderr,
        )
        return ""
    return mair_opt


def main() -> int:
    llvm_src = os.environ.get("LLVM_SRC")
    if not llvm_src:
        print("error: LLVM_SRC is not set", file=sys.stderr)
        return 1

    llvm = Path(llvm_src)
    if llvm.name != "llvm":
        llvm = llvm / "llvm"
    if not llvm.exists():
        print(f"{llvm} does not exists!", file=sys.stderr)
        return 1
    update_script = llvm / "utils" / "update_test_checks.py"
    if not update_script.exists():
        print(
            f"error: LLVM update_test_checks.py not found: {update_script}",
            file=sys.stderr,
        )
        return 1

    args = sys.argv[1:]
    forwarded_args = []
    if not has_arg(args, "--tool"):
        forwarded_args.append("--tool=mair-opt")
    if not has_arg(args, "--tool-binary") and not has_arg(args, "--opt-binary"):
        mair_opt = resolve_mair_opt_binary()
        if mair_opt == "":
            return 1
        if mair_opt:
            forwarded_args.append("--tool-binary=" + mair_opt)
    if not has_arg(args, "--check-inst-comments"):
        forwarded_args.append("--check-inst-comments")
    forwarded_args.extend(args)

    return subprocess.call([sys.executable, update_script, *forwarded_args])


if __name__ == "__main__":
    sys.exit(main())
