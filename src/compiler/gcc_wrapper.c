/**
 * @file gcc_wrapper.c
 * @brief A wrapper for GCC to integrate with the XSan project.
 *
 * Modified from the provided clang_wrapper.c.
 *
 * This wrapper parses compiler arguments to facilitate XSan's functionality
 * but, unlike the clang wrapper, it does NOT inject or manage any compiler
 * passes or plugins. It assumes that any necessary code instrumentation is
 * handled by a separate, modified GCC toolchain. Its main roles are to
 * detect sanitizer settings and manage the linking of appropriate XSan
 * runtime libraries.
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

#define WRAP_GCC
#include "xsan_wrapper_helper.c.inc"

static u8 AsmAsSource = 0; /* Whether to treat assembly files as source files */

#ifndef XSAN_PATH
#define XSAN_PATH ""
#endif

static u8 handle_asan_options(const char *arg, u8 is_neg) { return 0; }

static u8 handle_tsan_options(const char *arg, u8 is_neg) { return 0; }

static u8 handle_ubsan_options(const char *opt) { return 0; }

/*
  Ports the arguments for sanitizers to our plugin sanitizers
  Dicards the original argument if return 1.
*/
static u8 handle_sanitizer_options(const char *arg, enum SanitizerType sanTy) {
  OptAction action = handle_generic_sanitizer_options(arg, 1);
  if (action.act != OPT_PENDING)
    return action.act == OPT_REMOVE;
  arg = action.arg;
  u8 is_neg = action.is_neg;
  switch (sanTy) {
  case ASan:
    return handle_asan_options(arg, is_neg);
  case TSan:
    return handle_tsan_options(arg, is_neg);
  case UBSan:
    return handle_ubsan_options(arg);
  case XSan:
    if (has(&act_sanitizers, MSan)) {
      FATAL("'gcc-9.4.0 do not support 'msan'");
    }
    return handle_asan_options(arg, is_neg) | handle_tsan_options(arg, is_neg) |
           handle_ubsan_options(arg);
  case MSan:
    FATAL("gcc-9.4.0 do not support 'msan'");
  case SanNone:
  default:
    break;
  }

  return 0;
}

/* Copy argv to cc_params, making the necessary edits. */
static void edit_params(u32 argc, const char **argv) {
  u8 fortify_set = 0, asan_set = 0, x_set = 0, bit_mode = 0, shared_linking = 0,
     preprocessor_only = 0, have_unroll = 0, have_o = 0, have_pic = 0,
     have_c_or_S = 0, partial_linking = 0;
  u8 only_lib = 0, needs_shared_rt = 1;
  const u8 *name;
  enum SanitizerType xsanTy = SanNone;
  u8 is_cxx;
  int dump_active = 0;

  // Whether to use livepatch to handle link spec instead of custom specs
  // 1 = use livepatch for link spec; 0 = use specs (if enabled at build time)
  u8 livepatch_link_spec = 0;

  cc_params = ck_alloc((argc + 128) * sizeof(u8 *));

  name = strrchr(argv[0], '/');
  if (!name)
    name = argv[0];
  else
    name++;

  // Set the real compiler to 'gcc' or 'g++'
  // If the compiler is not 'gcc' or 'g++', we will use livepatch
  if (!strncmp(name, "xg++", strlen("xg++"))) {
    is_cxx = 1;
    u8 *alt_cxx = getenv("X_CXX");
    livepatch_link_spec = alt_cxx ? 1 : 0;
    cc_params[0] = alt_cxx ? alt_cxx : (u8 *)"g++";
  } else {
    is_cxx = 0;
    u8 *alt_cc = getenv("X_CC");
    livepatch_link_spec = alt_cc ? 1 : 0;
    cc_params[0] = alt_cc ? alt_cc : (u8 *)"gcc";
  }
  /* Detect the compilation mode in advance. */
  xsanTy = detect_san_type(argc, argv);

  for (u32 i = 1; i < argc; i++) {
    const u8 *cur = argv[i];

    if (!strcmp(cur, "-m32"))
      bit_mode = 32;
    else if (!strcmp(cur, "armv7a-linux-androideabi"))
      bit_mode = 32;
    else if (!strcmp(cur, "-m64"))
      bit_mode = 64;
    else if (handle_x_option((const u8 **)&argv[i], &AsmAsSource))
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
    else if (!strcmp(cur, "-c") || !strcmp(cur, "-S"))
      have_c_or_S = 1;
    else if (!strncmp(cur, "-O", 2))
      have_o = 1;
    else if (!strncmp(cur, "-funroll-loops", 14))
      have_unroll = 1;
    else if (!strcmp(cur, "-fdump-"))
      dump_active = 1;
    // If the user specifies custom specs, we will use livepatch
    else if (strncmp(cur, "-specs", 6) == 0) {
      WARNF("XSan: Custom specs may disable XSan functionality");
      livepatch_link_spec = 1;
    } else {
      OPT_EQ_AND_THEN(cur, "-static-libsan", {
        needs_shared_rt = 0;
        continue;
      })

      OPT_EQ_AND_THEN(cur, "-static-libasan", {
        if (xsanTy != SanNone)
          FATAL("The -xsan option is not compatible with -static-libasan.");

        needs_shared_rt = 0;
        continue;
      })

      OPT_EQ_AND_THEN(cur, "-static-libtsan", {
        if (xsanTy != SanNone)
          FATAL("The -xsan option is not compatible with -static-libtsan.");

        needs_shared_rt = 0;
        continue;
      })

      OPT_EQ_AND_THEN(cur, "-static-libubsan", {
        if (xsanTy != SanNone)
          FATAL("The -xsan option is not compatible with -static-libubsan.");

        needs_shared_rt = 0;
        continue;
      })

      OPT_EQ_AND_THEN(cur, "-lib-only", {
        only_lib = 1;
        continue;
      })

      // If source file is assembly, set AsmAsSource.
      if (AsmAsSource) {
        continue;
      }

      const char *suffix = strrchr(cur, '.');
      if (!suffix)
        continue;
      if ((!strcmp(suffix, ".s") || !strcmp(suffix, ".S") ||
           !strcmp(suffix, ".asm")) &&
          strcmp(argv[i - 1], "-o") /*Not as output*/) {
        AsmAsSource = 1;
        continue;
      }
    }
  }

#if XSAN_USE_GCC_SPEC
  // Configure livepatch based on build-time choice and runtime detection
  if (livepatch_link_spec) {
    setenv(XSAN_LIVEPATCH_GCC_LINK_SPEC_FLAG, "1", 1);
  } else {
    unsetenv(XSAN_LIVEPATCH_GCC_LINK_SPEC_FLAG);
  }
#else
  // When specs patch is disabled at build time, default to livepatch
  setenv(XSAN_LIVEPATCH_GCC_LINK_SPEC_FLAG, "1", 1);
#endif

  /// Set -x none for sanitizer runtime libraries.
  if (x_set) {
    cc_params[cc_par_cnt++] = "-x";
    cc_params[cc_par_cnt++] = "none";
  }

  // insure that the optimized file name is right
  if (dump_active) {
    cc_params[cc_par_cnt++] = "-dumpbase";
    cc_params[cc_par_cnt++] = "";
  }
  init_sanitizer_setting(xsanTy);

  /* Unlike clang_wrapper, we do NOT register a pass plugin for GCC. */
  /* The instrumentation is assumed to be handled by the patched GCC itself. */

  // Don't link runtime if it's a preprocessor-only run, or if there's no C/S
  // code, or if it's a partial link.
  if (!preprocessor_only && !have_c_or_S && !partial_linking) {
    cc_params[cc_par_cnt++] = "-B";
    cc_params[cc_par_cnt++] = obj_path;

#if XSAN_USE_GCC_SPEC
    // Use our custom specs to remove GCC default sanitizer linking
    // Only use specs if not falling back to livepatch
    if (!livepatch_link_spec) {
      cc_params[cc_par_cnt++] = "-specs";
      cc_params[cc_par_cnt++] =
          alloc_printf("%s/" XSAN_SHARE_DIR "/%s", obj_path,
                       is_cxx ? XSAN_SPEC_GPP_NAME : XSAN_SPEC_GCC_NAME);
    }
#endif
    add_sanitizer_runtime(xsanTy, is_cxx, shared_linking, needs_shared_rt);
  }

  while (--argc) {
    const u8 *cur = *(++argv);

    if (!strcmp(cur, "-Wl,-z,defs") || !strcmp(cur, "-Wl,--no-undefined"))
      continue;
    if (!strcmp(cur, "-static-libsan")) {
      continue;
    }
    if (handle_sanitizer_options(cur, xsanTy)) {
      continue;
    }

    cc_params[cc_par_cnt++] = cur;
  }

  if (getenv("X_HARDEN")) {
    cc_params[cc_par_cnt++] = "-fstack-protector-all";
    if (!fortify_set)
      cc_params[cc_par_cnt++] = "-D_FORTIFY_SOURCE=2";
  }

  if (!asan_set) {
    if (getenv("X_USE_ASAN")) {
      if (getenv("X_USE_MSAN"))
        FATAL("gcc-9.4.0 do not support MSAN");
      if (getenv("X_HARDEN"))
        FATAL("ASAN and X_HARDEN are mutually exclusive");
      cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
      cc_params[cc_par_cnt++] = "-fsanitize=address";
    } else if (getenv("X_USE_MSAN")) {
      FATAL("gcc-9.4.0 do not support MSAN");
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
  if (getenv("X_NO_BUILTIN")) {
    cc_params[cc_par_cnt++] = "-fno-builtin-strcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-memcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-bcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strstr";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasestr";
  }

  cc_params[cc_par_cnt] = NULL;
}

/* Print the command line for debugging. */
static void print_cmdline(int argc) {
  u32 i = 0;
  while (cc_params[i]) {
    printf("%s ", cc_params[i]);
    i++;
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
  }

  if (argc < 2) {

    SAYF(
        "\n"
        "This is a helper application for XSan. It serves as a drop-in "
        "replacement\n"
        "for gcc, letting you recompile third-party code with the required "
        "XSan\n"
        "runtime libraries and compiler features. A common use pattern would "
        "be:\n\n"

        "  CC=path/to/xgcc ./configure\n"
        "  CXX=path/to/xg++ ./configure\n\n"

        "This wrapper is designed to work with a modified GCC toolchain. The "
        "patched\n"
        "GCC handles the actual code instrumentation, while this program "
        "ensures\n"
        "the correct XSan runtime libraries are linked during the final "
        "stage.\n\n"

        "You can specify the path to the real compilers via the X_CC and "
        "X_CXX\n"
        "environment variables. Setting X_HARDEN enables additional hardening\n"
        "optimizations in the compiled code.\n\n");

    exit(1);
  }

#ifndef __ANDROID__
  find_obj((u8 *)argv[0]);
#endif

  edit_params(argc, argv);

  if (!!getenv("XGCC_DEBUG")) {
    print_cmdline(argc);
  }

  execvp(cc_params[0], (char **)cc_params);

  FATAL("Oops, failed to execute '%s' - check your PATH", cc_params[0]);

  return 0;
}
