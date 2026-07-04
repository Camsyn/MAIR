# -*- Python -*-

import os

import lit.formats

config.name = "MAIR"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ll"]
config.substitutions.append(("%mair-opt", config.mair_opt))
