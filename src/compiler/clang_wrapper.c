/**
 * Modified from AFL afl-clang-fast.c and Angora angora-clang.c,
 * as a wrapper for clang.
 *
 * TODO: use AFLplusplus's afl-cc.c as base, which is more powerful, supporting
 * LTO.
 */
#define AFL_MAIN

#include "config_compile.h"
#include "include/alloc-inl.h"
#include "include/debug.h"
#include "include/types.h"
#include "xsan_common.h"
#include "xsan_wrapper_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WRAP_CLANG
#include "xsan_wrapper_helper.c.inc"

#ifndef XSAN_PATH
#define XSAN_PATH ""
#endif

/* MODIFIED FROM AFL++。 TODO: Don't hard code embed linux */
/* Try to find a specific runtime we need, returns NULL on fail. */

struct FrontEndOpt {
  // Middle-end options could not transfer to the asm compilation.
  u8 AsmAsSource;
  /* -no-integrated-as , 0 dy default */
  u8 DisableIntegratedAS;
  u8 SanitizeAddressGlobalsDeadStripping;
  enum ObjectFormatType obj_format;
} frontend_opt = {.AsmAsSource = 0,
                  .DisableIntegratedAS = 0,
                  .SanitizeAddressGlobalsDeadStripping = 1,
// { MachO, COFF, ELF, GOFF, XCOFF, UnknownObjectFormat };
#if defined(__APPLE__)
                  .obj_format = MachO
#elif defined(__linux__)
                  .obj_format = ELF
#elif defined(_WIN32)
                  .obj_format = COFF
#else
                  .obj_format = UnknownObjectFormat
#endif

};

/*
 Handle the following options about ASan:
  -fsanitize-address-field-padding=<value>
  -fsanitize-address-globals-dead-stripping
  -fsanitize-address-poison-custom-array-cookie
  -fsanitize-address-use-after-return=<mode>
  -fsanitize-address-use-after-scope
  -fsanitize-address-use-odr-indicator
  -fsanitize-address-outline-instrumentation
  -fsanitize-recover=address|all
 Replace with
   -mllvm -xxx
  TODO: refer to SanitizerArgs.cpp:1193, to support
    1. -fsanitize=address,pointer-compare
    2. -fsanitize=address,pointer-subtract
 */
static u8 handle_asan_options(const char *arg, u8 is_mllvm_arg, u8 is_neg) {
  if (is_mllvm_arg) {
    if (!OPT_MATCH(arg, "-asan-")) {
      return 0;
    }
    arg += sizeof("-asan-") - 1;
    cc_params[cc_par_cnt++] = alloc_printf("-as-%s", arg);
    return 1;
  }

  if (!OPT_MATCH(arg, "address-")) {
    return 0;
  }
  arg += sizeof("address-") - 1;

  // -fsanitize-address-globals-dead-stripping
  OPT_EQ_AND_THEN(arg, "globals-dead-stripping", {
    frontend_opt.SanitizeAddressGlobalsDeadStripping = is_neg ? 0 : 1;
    return 0;
  })

  // -fsanitize-address-poison-custom-array-cookie
  OPT_EQ_AND_THEN(arg, "poison-custom-array-cookie", {
    // TODO
    return 0;
  })

  // -fsanitize-address-use-after-return=<mode>
  // frontend option, forward to middle-end option defined in PassRegistry.cpp
  OPT_GET_VAL_AND_THEN(arg, "use-after-return", {
    ADD_LLVM_MIDDLE_END_OPTION(
        alloc_printf("-sanitize-address-use-after-return=%s", val));
    return 0;
  })

  // -fsanitize-address-use-after-scope
  // frontend option, forward to middle-end option defined in PassRegistry.cpp
  OPT_EQ_AND_THEN(arg, "use-after-scope", {
    ADD_LLVM_MIDDLE_END_OPTION(is_neg ? "-sanitize-address-use-after-scope=0"
                                      : "-sanitize-address-use-after-scope");
    return 0;
  })

  // -fsanitize-address-use-odr-indicator
  // frontend option, forward to middle-end option defined in PassRegistry.cpp
  OPT_EQ_AND_THEN(arg, "use-odr-indicator", {
    ADD_LLVM_MIDDLE_END_OPTION(is_neg ? "-sanitize-address-use-odr-indicator=0"
                                      : "-sanitize-address-use-odr-indicator");
    return 0;
  })

  // -fsanitize-address-destructor=<value>
  // frontend option, forward to middle-end option defined in PassRegistry.cpp
  OPT_GET_VAL_AND_THEN(arg, "destructor", {
    ADD_LLVM_MIDDLE_END_OPTION(
        alloc_printf("-sanitize-address-destructor=%s", val));
    return 0;
  })

  // -fsanitize-address-outline-instrumentation
  OPT_EQ_AND_THEN(arg, "outline-instrumentation", {
    // clang driver translates this frontend option to middle-end options
    //          -mllvm -asan-instrumentation-with-call-threshold=0
    ADD_LLVM_MIDDLE_END_OPTION(
        is_neg ? "-as-instrumentation-with-call-threshold"
               : "-as-instrumentation-with-call-threshold=0");
    return 1;
  })

  return 0;
}

/*
 Handle the following options about TSan:
  -fsanitize-thread-atomics
  -fsanitize-thread-func-entry-exit
  -fsanitize-thread-memory-access
  -fsanitize-recover=address|all
 Replace with
   -mllvm -xxx
 Refer to SanitizerArgs.cpp:1142~1155
 */
static u8 handle_tsan_options(const char *arg, u8 is_mllvm_arg, u8 is_neg) {
  if (is_mllvm_arg) {
    if (!OPT_MATCH(arg, "-tsan-")) {
      return 0;
    }
    arg += sizeof("-tsan-") - 1;
    cc_params[cc_par_cnt++] = alloc_printf("-ts-%s", arg);
    return 1;
  }

  if (!OPT_MATCH(arg, "thread-")) {
    return 0;
  }
  arg += sizeof("thread-") - 1;

  // -fsanitize-thread-atomics
  OPT_EQ_AND_THEN(arg, "atomics", {
    // The clang driver translates this frontend option to middle-end options
    //          -mllvm -tsan-instrument-atomics=0
    if (!is_neg) {
      return 0;
    }
    ADD_LLVM_MIDDLE_END_OPTION("-ts-instrument-atomics=0");
    return 1;
  })

  // -fsanitize-thread-func-entry-exit
  OPT_EQ_AND_THEN(arg, "func-entry-exit", {
    // The clang driver translates this frontend option to middle-end options
    //          -mllvm -tsan-instrument-func-entry-exit=0
    if (!is_neg) {
      return 0;
    }
    ADD_LLVM_MIDDLE_END_OPTION("-ts-instrument-func-entry-exit=0");
    return 1;
  })

  // -fsanitize-thread-memory-access
  OPT_EQ_AND_THEN(arg, "memory-access", {
    // The clang driver translates this frontend option to middle-end options
    //          -mllvm -tsan-instrument-memory-accesses=0
    //          -mllvm -tsan-instrument-memintrinsics=0
    if (!is_neg) {
      return 0;
    }
    ADD_LLVM_MIDDLE_END_OPTION("-ts-instrument-memory-accesses=0");
    ADD_LLVM_MIDDLE_END_OPTION("-ts-instrument-memintrinsics=0");
    return 1;
  })

  return 0;
}

/*
 Handle the following options about ASan:
  -fsanitize-memory-track-origins=<value>
  -fsanitize-memory-track-origins
  -fsanitize-memory-param-retval
 Replace with
   -mllvm -xxx
 Not handle the following options because they are only used in CodeGen:
  -fsanitize-memory-use-after-dtor
 */
static u8 handle_msan_options(const char *arg, u8 is_mllvm_arg, u8 is_neg) {
  if (is_mllvm_arg) {
    if (!OPT_MATCH(arg, "-msan-")) {
      return 0;
    }
    arg += sizeof("-msan-") - 1;
    cc_params[cc_par_cnt++] = alloc_printf("-ms-%s", arg);
    return 1;
  }

  if (!OPT_MATCH(arg, "memory-")) {
    return 0;
  }
  arg += sizeof("memory-") - 1;

  // -fsanitize-memory-track-origins=<value>
  // -fsanitize-memory-track-origins
  // frontend option, forward to middle-end option defined in PassRegistry.cpp
  if (OPT_MATCH(arg, "track-origins")) {
    const char *end = arg + sizeof("track-origins") - 1;
    if (*end == 0) {
      ADD_LLVM_MIDDLE_END_OPTION(is_neg ? "-sanitize-memory-track-origins=0"
                                        : "-sanitize-memory-track-origins=2");
      return 1;
    } else if (*end == '=') {
      ADD_LLVM_MIDDLE_END_OPTION(
          alloc_printf("-sanitize-memory-track-origins=%s", end + 1));
      return 1;
    }
  }

  // -fsanitize-memory-param-retval
  // frontend option, forward to middle-end option defined in PassRegistry.cpp
  OPT_EQ_AND_THEN(arg, "param-retval", {
    ADD_LLVM_MIDDLE_END_OPTION(is_neg ? "-sanitize-memory-param-retval=0"
                                      : "-sanitize-memory-param-retval=1");
    return 1;
  })

  return 0;
}

static u8 handle_ubsan_options(const char *opt) { return 0; }

static u8 *fallback_if_missing(const char *path, u8 *fallback) {
  return path[0] && !access(path, X_OK) ? (u8 *)path : fallback;
}

/*
  Ports the arguments for sanitizers to our plugin sanitizers
  Dicards the original argument if return 1.
*/
static u8 handle_sanitizer_options(const char *arg, u8 is_mllvm_arg,
                                   enum SanitizerType sanTy) {
  OptAction action = handle_generic_sanitizer_options(arg, !is_mllvm_arg);
  if (action.act != OPT_PENDING)
    return action.act == OPT_REMOVE;
  arg = action.arg;
  u8 is_neg = action.is_neg;
  switch (sanTy) {
  case ASan:
    return handle_asan_options(arg, is_mllvm_arg, is_neg);
  case TSan:
    return handle_tsan_options(arg, is_mllvm_arg, is_neg);
  case MSan:
    return handle_msan_options(arg, is_mllvm_arg, is_neg);
  case UBSan:
    return handle_ubsan_options(arg);
  case XSan:
    return handle_asan_options(arg, is_mllvm_arg, is_neg) |
           handle_tsan_options(arg, is_mllvm_arg, is_neg) |
           handle_msan_options(arg, is_mllvm_arg, is_neg) |
           handle_ubsan_options(arg);
  case SanNone:
  default:
    break;
  }

  return 0;
}

static u8 asan_use_globals_gc() {
  /*
    static bool asanUseGlobalsGC(const Triple &T, const CodeGenOptions &CGOpts)
    { if (!CGOpts.SanitizeAddressGlobalsDeadStripping) return false; switch
    (T.getObjectFormat()) { case Triple::MachO: case Triple::COFF: return true;
      case Triple::ELF:
        return !CGOpts.DisableIntegratedAS;
      case Triple::GOFF:
        llvm::report_fatal_error("ASan not implemented for GOFF");
      case Triple::XCOFF:
        llvm::report_fatal_error("ASan not implemented for XCOFF.");
      case Triple::Wasm:
      case Triple::DXContainer:
      case Triple::SPIRV:
      case Triple::UnknownObjectFormat:
        break;
      }
      return false;
    }
  */
  if (!frontend_opt.SanitizeAddressGlobalsDeadStripping) {
    return 0;
  }
  switch (frontend_opt.obj_format) {
  case MachO:
  case COFF:
    return 1;
  case ELF:
    return !frontend_opt.DisableIntegratedAS;
  default:
    return 0;
  }
}

/* This function forwards the frontend options that could not handled by XSan to
 * the middle-end options that XSan can handle.The relevant middle-end options
 * are defined in instrumenation/PassRegistry.cpp */
static void add_pass_options(enum SanitizerType sanTy) {
  /// We only add pass options if there is any sanitizers in XSan-project
  /// activated.
  if (sanTy == SanNone)
    return;

  if (has(&act_sanitizers, ASan)) {
    if (!asan_use_globals_gc()) {
      ADD_LLVM_MIDDLE_END_OPTION("-asan-globals-gc=0");
    }

    if (has(&recover_sanitizers, ASan)) {
      ADD_LLVM_MIDDLE_END_OPTION("-sanitize-recover-address");
    }
  }
  if (has(&act_sanitizers, MSan)) {
    if (has(&recover_sanitizers, MSan)) {
      ADD_LLVM_MIDDLE_END_OPTION("-sanitize-recover-memory");
    }
  }
}

/// Pass is a specific feature for clang/LLVM
static void regist_pass_plugin(enum SanitizerType sanTy) {
  /**
   * Need to enable corresponding llvm optimization level,
   * where your pass is registed.
   */
  u8 *san_pass = "";
  switch (sanTy) {
  case ASan:
    san_pass = "ASanInstPass.so";
    break;
  case TSan:
    san_pass = "TSanInstPass.so";
    break;
  case MSan:
    san_pass = "MSanInstPass.so";
    break;
  case XSan:
    san_pass = "XSanInstPass.so";
    break;
  case UBSan:
  case SanNone:
  default:
    return;
  }

  /* The relevant code in Frontend/CompilerInvocation.cpp:CreateFromArgsImpl
    // FIXME: Override value name discarding when asan or msan is used because
    the
    // backend passes depend on the name of the alloca in order to print out
    // names.
    Res.getCodeGenOpts().DiscardValueNames &=
        !LangOpts.Sanitize.has(SanitizerKind::Address) &&
        !LangOpts.Sanitize.has(SanitizerKind::KernelAddress) &&
        !LangOpts.Sanitize.has(SanitizerKind::Memory) &&
        !LangOpts.Sanitize.has(SanitizerKind::KernelMemory)
   */
  cc_params[cc_par_cnt++] = "-fno-discard-value-names";

  san_pass = alloc_printf("%s/" XSAN_PASS_DIR "/%s", obj_path, san_pass);

  /*
    From this issue https://github.com/llvm/llvm-project/issues/56137
    To pass the option to plugin pass, we need add extra options
  */
  // cc_params[cc_par_cnt++] = "-Xclang";
  // cc_params[cc_par_cnt++] = "-load";
  // cc_params[cc_par_cnt++] = "-Xclang";
  // cc_params[cc_par_cnt++] = san_pass;
  // cc_params[cc_par_cnt++] = "-Xclang";
  cc_params[cc_par_cnt++] = alloc_printf("-fplugin=%s", san_pass);
  cc_params[cc_par_cnt++] = alloc_printf("-fpass-plugin=%s", san_pass);

  /**
   * Transfer the option to pass by `-mllvm -<opt>`
   */
  if (sanTy != XSan) {
    return;
  }
  if (!has(&act_sanitizers, ASan)) {
    ADD_LLVM_MIDDLE_END_OPTION("-xsan-disable-asan");
  }
  if (!has(&act_sanitizers, TSan)) {
    ADD_LLVM_MIDDLE_END_OPTION("-xsan-disable-tsan");
  }
  if (!has(&act_sanitizers, MSan)) {
    ADD_LLVM_MIDDLE_END_OPTION("-xsan-disable-msan");
  }
}

/* Copy argv to cc_params, making the necessary edits. */
static void edit_params(u32 argc, const char **argv) {
  /// TODO:
  /*
  How to resolve the arg via libclang?
  See clang-tool-extra,
  findInputFile(const CommandLineArguments &CLArgs),
  getDriverOptTable().ParseArgs(
  */
  u8 fortify_set = 0, asan_set = 0, x_set = 0, bit_mode = 0, shared_linking = 0,
     preprocessor_only = 0, have_unroll = 0, have_o = 0, have_pic = 0,
     have_c = 0, partial_linking = 0;
  u8 only_lib = 0, needs_shared_rt = 0;
  const u8 *name;
  enum SanitizerType xsanTy = SanNone;
  u8 is_cxx;

  cc_params = ck_alloc((argc + 256) * sizeof(u8 *));

  name = strrchr(argv[0], '/');
  if (!name)
    name = argv[0];
  else
    name++;

  if (!strncmp(name, "xclang++", strlen("xclang++"))) {
    is_cxx = 1;
    u8 *alt_cxx = getenv("X_CXX");
    cc_params[0] =
        alt_cxx ? alt_cxx
                : fallback_if_missing(XSAN_DEFAULT_CLANGXX, (u8 *)"clang++");
  } else {
    is_cxx = 0;
    u8 *alt_cc = getenv("X_CC");
    cc_params[0] = alt_cc
                       ? alt_cc
                       : fallback_if_missing(XSAN_DEFAULT_CLANG, (u8 *)"clang");
  }

  /* There are two ways to compile afl-clang-fast. In the traditional mode, we
     use afl-llvm-pass.so to inject instrumentation. In the experimental
     'trace-pc-guard' mode, we use native LLVM instrumentation callbacks
     instead. The latter is a very recent addition - see:

     http://clang.llvm.org/docs/SanitizerCoverage.html#tracing-pcs-with-guards
   */

  /*#ifdef USE_TRACE_PC
    cc_params[cc_par_cnt++] = "-fsanitize-coverage=trace-pc-guard";
  #ifndef __ANDROID__
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = -sanitize"r-coverage-block-threshold=0";
  #endif
  #else
    cc_params[cc_par_cnt++] = "-Xclang";
    cc_params[cc_par_cnt++] = "-load";
    cc_params[cc_par_cnt++] = "-Xclang";
    cc_params[cc_par_cnt++] = alloc_printf("%s/afl-llvm-pass.so", obj_path);
  #endif*/ /* ^USE_TRACE_PC */

  cc_params[cc_par_cnt++] = "-Qunused-arguments";

  /* Detect the compilation mode in advance. */
  xsanTy = detect_san_type(argc, argv);
  for (u32 i = 1; i < argc; i++) {
    const u8 *cur = argv[i];

    if (!strcmp(cur, "--driver-mode=g++"))
      is_cxx = 1;
    else if (!strcmp(cur, "-m32"))
      bit_mode = 32;
    else if (!strcmp(cur, "armv7a-linux-androideabi"))
      bit_mode = 32;
    else if (!strcmp(cur, "-m64"))
      bit_mode = 64;
    else if (handle_x_option((const u8 **)&argv[i], &frontend_opt.AsmAsSource))
      x_set = 1;
    else if (!strcmp(cur, "-fsanitize=address") ||
             !strcmp(cur, "-fsanitize=memory"))
      asan_set = 1;
    else if (strstr(cur, "FORTIFY_SOURCE"))
      fortify_set = 1;
    else if (!strcmp(cur, "-E"))
      preprocessor_only = 1;
    else if (!strcmp(cur, "-shared"))
      shared_linking = 1;
    else if (!strcmp(cur, "-dynamiclib"))
      shared_linking = 1;
    else if (!strcmp(cur, "-Wl,-r"))
      partial_linking = 1;
    else if (!strcmp(cur, "-Wl,-i"))
      partial_linking = 1;
    else if (!strcmp(cur, "-Wl,--relocatable"))
      partial_linking = 1;
    else if (!strcmp(cur, "-r"))
      partial_linking = 1;
    else if (!strcmp(cur, "--relocatable"))
      partial_linking = 1;
    else if (!strcmp(cur, "-c"))
      have_c = 1;
    else if (!strncmp(cur, "-O", 2))
      have_o = 1;
    else if (!strncmp(cur, "-funroll-loops", 14))
      have_unroll = 1;
    else {
      OPT_EQ_AND_THEN(cur, "-shared-libsan", {
        if (xsanTy == XSan && has(&act_sanitizers, MSan)) {
          PFATAL(
              "XSan with MSan does not support the shared runtime "
              "temporarily, as MSan does not support it either. Consider "
              "disable MSan from XSan if you want to use the shared runtime.");
        }
        needs_shared_rt = 1;
        continue;
      })

      OPT_EQ_AND_THEN(cur, "-lib-only", {
        only_lib = 1;
        continue;
      })

      // For ASan's global gc option.
      // Search "-asan-globals-gc=0" in this file for details.
      OPT_EQ_AND_THEN(cur, "-no-integrated-as", {
        frontend_opt.DisableIntegratedAS = 1;
        continue;
      })
      OPT_GET_VAL_AND_THEN(cur, "--target", {
        if (strstr(val, "apple")) {
          frontend_opt.obj_format = MachO;
        } else if (strstr(val, "linux")) {
          frontend_opt.obj_format = ELF;
        } else if (strstr(val, "mingw32")) {
          frontend_opt.obj_format = COFF;
        } else {
          frontend_opt.obj_format = UnknownObjectFormat;
        }
        continue;
      })

      // If source file is assembly, set AsmAsSource.
      if (frontend_opt.AsmAsSource) {
        continue;
      }
      // Clang has a list:
      // https://github.com/llvm/llvm-project/blob/b9f3b7f89a4cb4cf541b7116d9389c73690f78fa/clang/lib/Driver/Types.cpp#L293
      // assembly_source_extensions = ('.s', '.asm')
      // assembly_needing_c_preprocessor_source_extensions = ('.S')
      // Check the suffix
      const char *suffix = strrchr(cur, '.');
      if (!suffix)
        continue;
      if ((!strcmp(suffix, ".s") || !strcmp(suffix, ".S") ||
           !strcmp(suffix, ".asm")) &&
          strcmp(argv[i - 1], "-o") /*Not as output*/) {
        frontend_opt.AsmAsSource = 1;
        continue;
      }
    }
  }

  /// Set -x none for sanitizer rutnime libraries.
  if (x_set) {
    cc_params[cc_par_cnt++] = "-x";
    cc_params[cc_par_cnt++] = "none";
  }

  init_sanitizer_setting(xsanTy);

  /*
    We should put the sanitizer static runtime library just ahead of the
    input xxx.o/xxx.c  files, so that the CTor of sanitizers can be called
    before the CTors of user code (.preinit, module_ctor).

    What's more, the compiler also places the sanitizer runtime library before
    the input files after the Driver generates the command line, which should be
    adhered by this compiler wrapper.

    However, it is hard to judge whether an argument is an input file or not
    precisely, so we simply add the sanitizer runtime library as front as
    possible.

    See ASan's testcase "init_fini_sections.cpp" for details.
  */
  if (!only_lib)
    regist_pass_plugin(xsanTy);

  // *.c/cpp -o *.o, don't link sanitizer runtime library.
  if (!have_c)
    add_sanitizer_runtime(xsanTy, is_cxx, shared_linking, needs_shared_rt);

  while (--argc) {
    const u8 *cur = *(++argv);

    if (!strcmp(cur, "-Wl,-z,defs") || !strcmp(cur, "-Wl,--no-undefined"))
      continue;

    if (handle_sanitizer_options(cur, !strcmp(argv[-1], "-mllvm"), xsanTy))
      continue;

    if (frontend_opt.AsmAsSource && !strcmp(cur, "-mllvm")) {
      ++argv;
      --argc;
      continue;
    }

    cc_params[cc_par_cnt++] = cur;
  }

  if (!only_lib)
    add_pass_options(xsanTy);

  if (getenv("X_HARDEN")) {

    cc_params[cc_par_cnt++] = "-fstack-protector-all";

    if (!fortify_set)
      cc_params[cc_par_cnt++] = "-D_FORTIFY_SOURCE=2";
  }

  if (!asan_set) {

    if (getenv("X_USE_ASAN")) {

      if (getenv("X_USE_MSAN"))
        FATAL("ASAN and MSAN are mutually exclusive");

      if (getenv("X_HARDEN"))
        FATAL("ASAN and X_HARDEN are mutually exclusive");

      cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
      cc_params[cc_par_cnt++] = "-fsanitize=address";

    } else if (getenv("X_USE_MSAN")) {

      if (getenv("X_USE_ASAN"))
        FATAL("ASAN and MSAN are mutually exclusive");

      if (getenv("X_HARDEN"))
        FATAL("MSAN and X_HARDEN are mutually exclusive");

      cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
      cc_params[cc_par_cnt++] = "-fsanitize=memory";
    }
  }

#ifdef USE_TRACE_PC

  if (getenv("X_INST_RATIO"))
    FATAL("X_INST_RATIO not available at compile time with 'trace-pc'.");

#endif /* USE_TRACE_PC */

  /**
   * Turn off builtin functions.
   * TODO: support LTO
   */
  if (getenv("X_NO_BUILTIN") /* || lto_mode */) {

    cc_params[cc_par_cnt++] = "-fno-builtin-strcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-memcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-bcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strstr";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasestr";
  }

  if (!have_c) {
    cc_params[cc_par_cnt++] = "-fuse-ld=lld";
    // /usr/local/lib is not the default search path for lld, so we add it here.
    cc_params[cc_par_cnt++] = "-L/usr/local/lib";
  }

  /*#ifndef __ANDROID__
    switch (bit_mode) {

      case 0:
        cc_params[cc_par_cnt++] = alloc_printf("%s/afl-llvm-rt.o", obj_path);
        break;

      case 32:
        cc_params[cc_par_cnt++] = alloc_printf("%s/afl-llvm-rt-32.o", obj_path);

        if (access(cc_params[cc_par_cnt - 1], R_OK))
          FATAL("-m32 is not supported by your compiler");

        break;

      case 64:
        cc_params[cc_par_cnt++] = alloc_printf("%s/afl-llvm-rt-64.o", obj_path);

        if (access(cc_params[cc_par_cnt - 1], R_OK))
          FATAL("-m64 is not supported by your compiler");

        break;

    }
  #endif*/

  cc_params[cc_par_cnt] = NULL;
}

static void print_cmdline(int argc) {

  for (int i = 0; i < argc + cc_par_cnt; i++) {
    if (cc_params[i] == NULL)
      break;
    printf("%s ", cc_params[i]);
  }
  printf("\n");
}

/* Main entry point */

int main(int argc, const char **argv) {

  if (argc == 2 && strcmp(argv[1], "-h") == 0) {
    print_help();
    return 0;
  }

  if (isatty(2) && !getenv("AFL_QUIET")) {

    /*#ifdef USE_TRACE_PC
        SAYF(cCYA "afl-clang-fast [tpcg] " cBRI VERSION  cRST " by
    <lszekeres@google.com>\n"); #else SAYF(cCYA "afl-clang-fast " cBRI VERSION
    cRST " by <lszekeres@google.com>\n"); #endif*/ /* ^USE_TRACE_PC */
  }

  if (argc < 2) {

    SAYF("\n"
         "This is a helper application for XSan. It serves as a drop-in "
         "replacement\n"
         "for clang, letting you recompile third-party code with the required "
         "runtime\n"
         "instrumentation. A common use pattern would be one of the "
         "following:\n\n"

         "  CC=path/xclang ./configure\n"
         "  CXX=path/xclang++ ./configure\n\n"

         "In contrast to the traditional clang tool, this version is "
         "implemented as\n"
         "an LLVM pass and tends to offer improved performance with slow "
         "programs.\n\n"

         "You can specify custom next-stage toolchain via X_CC and X_CXX. "
         "Setting\n"
         "X_HARDEN enables hardening optimizations in the compiled code.\n\n");

    exit(1);
  }

#ifndef __ANDROID__
  find_obj((u8 *)argv[0]);
#endif

  edit_params(argc, argv);

  if (!!getenv("XCLANG_DEBUG")) {
    print_cmdline(argc);
  }

  execvp(cc_params[0], (char **)cc_params);

  FATAL("Oops, failed to execute '%s' - check your PATH", cc_params[0]);

  return 0;
}
