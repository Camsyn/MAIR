# -*- Python -*-

import os

import lit.formats

config.name = "MAIR"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ll"]
config.environment["PATH"] = os.path.pathsep.join(
    (os.path.dirname(config.mair_opt), config.environment["PATH"])
)
config.substitutions.append(("mair_opt", config.mair_opt))
config.substitutions.append(("mair-opt", config.mair_opt))
