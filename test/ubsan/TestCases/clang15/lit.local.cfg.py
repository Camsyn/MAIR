import re
import subprocess


def getRoot(config):
    if not config.parent:
        return config
    return getRoot(config.parent)


def getClangMajor(clang):
    clang = clang.strip()
    version = subprocess.check_output([clang, "--version"], text=True)
    match = re.search(r"clang version (\d+)", version)
    if not match:
        lit_config.fatal("Failed to detect clang version from %r" % clang)
    return int(match.group(1))


root = getRoot(config)

if getClangMajor(root.clang) != 15:
    config.unsupported = True
