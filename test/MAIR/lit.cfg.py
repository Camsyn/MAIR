# -*- Python -*-

import os
import subprocess

import lit.formats

config.name = "MAIR"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ll"]
config.environment["PATH"] = os.path.pathsep.join(
    (os.path.dirname(config.mair_opt), config.environment["PATH"])
)
config.substitutions.append(("mair_opt", config.mair_opt))
config.substitutions.append(("mair-opt", config.mair_opt))

stats_probe = subprocess.run(
    [
        config.mair_opt,
        "-stats",
        "--disable-output",
        "-passes=simplifygraph",
        "-",
    ],
    input="define void @f() { ret void }\n",
    text=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.PIPE,
    env=config.environment,
    check=False,
)
if (
    stats_probe.returncode == 0
    and stats_probe.stderr
    and "Statistics are disabled" not in stats_probe.stderr
):
    config.available_features.add("mair_stats")
