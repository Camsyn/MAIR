This directory is the root for running XSan on 50+ real-world projects.

- `<package>`: package under test
    - [manual] `fetch.sh`: fetch the package source while keeping it small; for git clone, prefer `--depth=1`.
    - [manual] `compile.sh`: build script controlled by environment variables; avoid unrelated build outputs and move binaries into `artefacts`.
        ```requirements
        Add `-g` by default, support parallel builds, and avoid depending on package-local shared libraries when possible.
        - Support combinations in P({TSan, UBSan, MSan}) + ASan (8 modes total).
        - Support native ASan, MSan, TSan, and UBSan build modes.
        - Support a no-sanitizer baseline.
        - Support `-O0` builds.
        ```
    - repo: package source; keep it small, for example by using `git clone --depth=1`.
    - temp: temporary directory used by `compile.sh`.
    - artefact
        - `<program1>`: built executable name
        - `<program2>`: built executable name
          - [manual] `run.sh`: run script for the target program; it must set `PRE_OPT`, `POST_OPT`, `stdin`, and `PROG_DIR`.
            - Note: `run.sh` must be suitable for large-scale runs. It should not leave junk files in the working directory or modify inputs in place. For decompression tools, write extracted output to a temporary directory under `/tmp`.
          - `<prog>.dbg`: debug build
          - `<prog>.raw`: no-sanitizer baseline
          - `<prog>.asan`: ASan
          - `<prog>.tsan`: TSan
          - `<prog>.ubsan`: UBSan
          - `<prog>.msan`: MSan
          - `<prog>.xsan-asan`: ASan@XSan
          - `<prog>.xsan-asan-msan`: (ASan+MSan)@XSan
          - `<prog>.xsan-asan-tsan`: (ASan+TSan)@XSan
          - `<prog>.xsan-asan-ubsan`: (ASan+UBSan)@XSan
          - `<prog>.xsan-asan-msan-tsan`: (ASan+MSan+TSan)@XSan
          - `<prog>.xsan-asan-msan-ubsan`: (ASan+MSan+UBSan)@XSan
          - `<prog>.xsan-asan-tsan-ubsan`: (ASan+TSan+UBSan)@XSan
          - `<prog>.xsan-asan-msan-tsan-ubsan`: (ASan+MSan+TSan+UBSan)@XSan
    - inputs: test inputs, either from fuzzing seed corpora or package-provided tests
        - `<program1>`: a directory containing one or more test inputs for program1
        - `<program2>`: if program2 shares inputs with program1, use `ln -s program1 program2`.
- `<pakcage>`: ...
  - ...
- `activate_compile_flags.sh`: sourced by `compile.sh`.
- `compile-all.sh`: calls `compile.sh` to build all Sanitizer modes.
- `run.sh`: shared run script, called only by `artefacts/<prog>/run.sh`.
- `common-run.sh`: common runtime helper called by `run.sh`.
- `eval-sample.sh`: single-run measurement script for performance and memory usage.
- `benchmarking.py`: automates performance and memory evaluation across Sanitizer configurations and stores results for later analysis.
- `benchmarks.py`: defines benchmark programs and Sanitizer configuration sets.
- `gen_bench_config.py`: generates the `benchmarks.py` configuration file.
- `README.md`: this file

Example:
```
.
├── binutils
│   ├── artefacts
│   │   ├── cxxfilt
│   │   │   ├── cxxfilt.asan
│   │   │   ├── cxxfilt.msan
│   │   │   ├── cxxfilt.raw
│   │   │   ├── cxxfilt.tsan
│   │   │   ├── cxxfilt.ubsan
│   │   │   ├── cxxfilt.xsan-asan
│   │   │   ├── cxxfilt.xsan-asan-msan
│   │   │   ├── cxxfilt.xsan-asan-msan-tsan
│   │   │   ├── cxxfilt.xsan-asan-msan-tsan-ubsan
│   │   │   ├── cxxfilt.xsan-asan-msan-ubsan
│   │   │   ├── cxxfilt.xsan-asan-tsan
│   │   │   ├── cxxfilt.xsan-asan-tsan-ubsan
│   │   │   └── cxxfilt.xsan-asan-ubsan
│   │   ├── objcopy
│   │   │   ├── objcopy.asan
│   │   │   ├── objcopy.msan
│   │   │   ├── objcopy.raw
│   │   │   ├── objcopy.tsan
│   │   │   ├── objcopy.ubsan
│   │   │   ├── objcopy.xsan-asan
│   │   │   ├── objcopy.xsan-asan-msan
│   │   │   ├── objcopy.xsan-asan-msan-tsan
│   │   │   ├── objcopy.xsan-asan-msan-tsan-ubsan
│   │   │   ├── objcopy.xsan-asan-msan-ubsan
│   │   │   ├── objcopy.xsan-asan-tsan
│   │   │   ├── objcopy.xsan-asan-tsan-ubsan
│   │   │   └── objcopy.xsan-asan-ubsan
│   │   ├── objdump
│   │   │   ├── objdump.asan
│   │   │   ├── objdump.msan
│   │   │   ├── objdump.raw
│   │   │   ├── objdump.tsan
│   │   │   ├── objdump.ubsan
│   │   │   ├── objdump.xsan-asan
│   │   │   ├── objdump.xsan-asan-msan
│   │   │   ├── objdump.xsan-asan-msan-tsan
│   │   │   ├── objdump.xsan-asan-msan-tsan-ubsan
│   │   │   ├── objdump.xsan-asan-msan-ubsan
│   │   │   ├── objdump.xsan-asan-tsan
│   │   │   ├── objdump.xsan-asan-tsan-ubsan
│   │   │   └── objdump.xsan-asan-ubsan
│   │   ├── readelf
│   │   │   ├── readelf.asan
│   │   │   ├── readelf.msan
│   │   │   ├── readelf.raw
│   │   │   ├── readelf.tsan
│   │   │   ├── readelf.ubsan
│   │   │   ├── readelf.xsan-asan
│   │   │   ├── readelf.xsan-asan-msan
│   │   │   ├── readelf.xsan-asan-msan-tsan
│   │   │   ├── readelf.xsan-asan-msan-tsan-ubsan
│   │   │   ├── readelf.xsan-asan-msan-ubsan
│   │   │   ├── readelf.xsan-asan-tsan
│   │   │   ├── readelf.xsan-asan-tsan-ubsan
│   │   │   └── readelf.xsan-asan-ubsan
│   │   ├── size
│   │   │   ├── size.asan
│   │   │   ├── size.msan
│   │   │   ├── size.raw
│   │   │   ├── size.tsan
│   │   │   ├── size.ubsan
│   │   │   ├── size.xsan-asan
│   │   │   ├── size.xsan-asan-msan
│   │   │   ├── size.xsan-asan-msan-tsan
│   │   │   ├── size.xsan-asan-msan-tsan-ubsan
│   │   │   ├── size.xsan-asan-msan-ubsan
│   │   │   ├── size.xsan-asan-tsan
│   │   │   ├── size.xsan-asan-tsan-ubsan
│   │   │   └── size.xsan-asan-ubsan
│   │   ├── strings
│   │   │   ├── strings.asan
│   │   │   ├── strings.msan
│   │   │   ├── strings.raw
│   │   │   ├── strings.tsan
│   │   │   ├── strings.ubsan
│   │   │   ├── strings.xsan-asan
│   │   │   ├── strings.xsan-asan-msan
│   │   │   ├── strings.xsan-asan-msan-tsan
│   │   │   ├── strings.xsan-asan-msan-tsan-ubsan
│   │   │   ├── strings.xsan-asan-msan-ubsan
│   │   │   ├── strings.xsan-asan-tsan
│   │   │   ├── strings.xsan-asan-tsan-ubsan
│   │   │   └── strings.xsan-asan-ubsan
│   │   └── strip-new
│   │       ├── strip-new.asan
│   │       ├── strip-new.msan
│   │       ├── strip-new.raw
│   │       ├── strip-new.tsan
│   │       ├── strip-new.ubsan
│   │       ├── strip-new.xsan-asan
│   │       ├── strip-new.xsan-asan-msan
│   │       ├── strip-new.xsan-asan-msan-tsan
│   │       ├── strip-new.xsan-asan-msan-tsan-ubsan
│   │       ├── strip-new.xsan-asan-msan-ubsan
│   │       ├── strip-new.xsan-asan-tsan
│   │       ├── strip-new.xsan-asan-tsan-ubsan
│   │       └── strip-new.xsan-asan-ubsan
│   ├── compile.sh
│   ├── fetch.sh
│   ├── repo
│   └── temp
├── activate_compile_flags.sh
├── compile-all.sh
├── ...
└── README.md

```