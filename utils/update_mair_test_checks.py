#!/usr/bin/env python3

import os
import shutil
import shlex
import subprocess
import sys
from pathlib import Path


def has_arg(args, name: str) -> bool:
    return any(arg == name or arg.startswith(name + "=") for arg in args)


def get_arg_value(args: list[str], *names: str) -> str | None:
    for index, arg in enumerate(args):
        for name in names:
            if arg == name and index + 1 < len(args):
                return args[index + 1]
            if arg.startswith(name + "="):
                return arg.split("=", 1)[1]
    return None


def get_test_paths(args: list[str]) -> list[Path]:
    return [Path(arg) for arg in args if arg.endswith(".ll") and os.path.isfile(arg)]


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


def get_mair_opt_for_execution(args: list[str]) -> str | None:
    explicit_binary = get_arg_value(args, "--tool-binary", "--opt-binary")
    if explicit_binary:
        return os.path.abspath(os.path.expanduser(explicit_binary))

    mair_opt = resolve_mair_opt_binary()
    if mair_opt == "":
        return None
    return mair_opt or "mair-opt"


def remove_emit_llvm_option(tokens: list[str]) -> list[str]:
    return [token for token in tokens if token != "-emit-llvm"]


def get_emit_llvm_run_command(test_path: Path) -> str | None:
    for line in test_path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped.startswith("; RUN:"):
            continue
        command = stripped[len("; RUN:") :].strip()
        tool_command = command.split("|", 1)[0].strip()
        if not tool_command.startswith("mair-opt "):
            continue
        tokens = shlex.split(tool_command)
        if "-emit-llvm" in tokens:
            return tool_command
    return None


def mair_golden_tokens_for(test_path: Path) -> list[str]:
    emit_llvm_command = get_emit_llvm_run_command(test_path)
    if not emit_llvm_command:
        print(
            f"error: no mair-opt -emit-llvm RUN line found in {test_path}",
            file=sys.stderr,
        )
        raise SystemExit(1)

    return remove_emit_llvm_option(shlex.split(emit_llvm_command))


def mair_golden_command_for(test_path: Path) -> str:
    return shlex.join(mair_golden_tokens_for(test_path))


def mair_golden_run_for(test_path: Path) -> str:
    command = mair_golden_command_for(test_path)
    return f"; RUN: {command} > %t.ma && diff -u %t.ma %S/mair/{test_path.stem}.ma"


def is_mair_golden_run_line(line: str) -> bool:
    stripped = line.rstrip("\n")
    return (
        stripped.startswith("; RUN: mair-opt ")
        and " > %t.ma && diff -u %t.ma %S/mair/" in stripped
        and stripped.endswith(".ma")
    )


def strip_mair_golden_run(test_path: Path) -> bool:
    lines = test_path.read_text(encoding="utf-8").splitlines(keepends=True)
    kept_lines = [line for line in lines if not is_mair_golden_run_line(line)]
    if len(kept_lines) == len(lines):
        return False
    test_path.write_text("".join(kept_lines), encoding="utf-8")
    return True


def ensure_mair_golden_run(test_path: Path) -> None:
    run_line = mair_golden_run_for(test_path)
    lines = test_path.read_text(encoding="utf-8").splitlines(keepends=True)
    if any(line.rstrip("\n") == run_line for line in lines):
        return

    for index, line in enumerate(lines):
        stripped = line.strip()
        command = stripped[len("; RUN:") :].strip() if stripped.startswith("; RUN:") else ""
        tool_command = command.split("|", 1)[0].strip()
        if tool_command.startswith("mair-opt ") and "-emit-llvm" in shlex.split(
            tool_command
        ):
            lines.insert(index + 1, run_line + "\n")
            test_path.write_text("".join(lines), encoding="utf-8")
            return

    print(
        f"error: no mair-opt -emit-llvm RUN line found in {test_path}",
        file=sys.stderr,
    )
    raise SystemExit(1)


def update_mair_golden(test_path: Path, mair_opt: str) -> None:
    golden_dir = test_path.parent / "mair"
    golden_dir.mkdir(parents=True, exist_ok=True)
    golden_path = golden_dir / (test_path.stem + ".ma")
    tokens = mair_golden_tokens_for(test_path)
    tokens[0] = mair_opt
    tokens = [str(test_path) if token == "%s" else token for token in tokens]
    output = subprocess.check_output(tokens)
    golden_path.write_bytes(output)


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
    test_paths = get_test_paths(args)
    mair_opt_for_golden = get_mair_opt_for_execution(args)
    if mair_opt_for_golden is None:
        return 1

    forwarded_args = []
    if not has_arg(args, "--tool"):
        forwarded_args.append("--tool=mair-opt")
    if not has_arg(args, "--tool-binary") and not has_arg(args, "--opt-binary"):
        if mair_opt_for_golden != "mair-opt":
            forwarded_args.append("--tool-binary=" + mair_opt_for_golden)
    if not has_arg(args, "--check-inst-comments"):
        forwarded_args.append("--check-inst-comments")
    forwarded_args.extend(args)

    had_golden_run = {path: strip_mair_golden_run(path) for path in test_paths}
    result = subprocess.call([sys.executable, update_script, *forwarded_args])
    if result != 0:
        for path, existed in had_golden_run.items():
            if existed:
                ensure_mair_golden_run(path)
        return result

    for path in test_paths:
        ensure_mair_golden_run(path)
        update_mair_golden(path, mair_opt_for_golden)
    return 0


if __name__ == "__main__":
    sys.exit(main())
