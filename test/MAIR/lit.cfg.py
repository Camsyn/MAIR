# -*- Python -*-

import os

import lit.formats

config.name = "MAIR"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ll"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(os.path.dirname(__file__), "Output")
config.substitutions.append(("%mair_opt", config.mair_opt))
