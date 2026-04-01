/*
Patch the error_at in diagnostic.c/diagnostic.cc in gcc.
*/
#include <cstdarg>
#include <cstdint>
#include <string.h>

#include "../utils/PatchHelper.h"
#include "llvm/ADT/STLExtras.h"
#include <llvm/ADT/StringRef.h>

// This type definition comes from gcc's unexported header file:
// libcpp/include/line-map.h
using location_t = unsigned int;

void error_at(location_t loc, const char *gmsgid, ...);
static __xsan::XsanInterceptor
    Interceptor(&error_at,
                /* In fact, cc1 is enough, our match is a match, just
                   to clarify the semantics, plus cc1plus */
                {"cc1", "cc1plus", /* For LTO */ "lto1"});

static unsigned count_gcc_diag_specifiers(const char *gmsgid,
                                          std::string_view given_spec = "");

// template <size_t> intptr_t get_arg(va_list &args) {
//   return va_arg(args, intptr_t);
// }
// // Use varargs template to simplify the repetitive code
// template <size_t... Is>
// void call_interceptor(location_t loc, const char *gmsgid, va_list &args,
//                       std::index_sequence<Is...>) {
//   Interceptor(loc, gmsgid, get_arg<Is>(args)...);
// }

/// A better implementation to expand the variadic parameters
/// I.e., use the comma expression.
template <size_t... Is>
void call_interceptor(location_t loc, const char *gmsgid, va_list &args,
                      std::index_sequence<Is...>) {
  Interceptor(loc, gmsgid,
              (/* Suppress the compiler warning */ static_cast<void>(Is),
               va_arg(args, intptr_t))...);
}

// This symbol exists on cc1's dynsym table.
/* Same as above, but use location LOC instead of input_location.  */
/// TODO: more robust implementation.
/*
This symbol is used in gcc for error reporting and program termination.
Theoretically, by intercepting this function, we can allow 'incompatible'
compilation options like -fsanitize=address,thread.

Fortunately, this symbol exists in dynsym, i.e., the dynamic symbol table,
and we can get the address of this symbol through dlsym.
Unfortunately:
1. gcc doesn't depend on dynamic libraries, i.e., we can't directly inject DLL.
2. This function is directly located in the executable, and will even shadow
   the same-named symbols in dynamic libraries loaded via LD_PRELOAD.
3. This function is a C-style varargs function, and C/C++ cannot implement
   safe variable-length parameter forwarding (because the number of parameters
   needs to be determined at compile time).

A sound approach to intercepting variadic functions is to share the same stack
frame, which requires handwritten assembly (see comments at the end of this file
for details).

This function avoids handwritten assembly by analyzing all usage cases of
error_at and GCC diagnostic specifier rules.
We finally dynamically count specifiers in the format string to determine the
number of parameters, and uniformly use intptr_t type to pass parameters, thus
avoiding handwritten assembly. This approach cannot be flexibly extended to
arbitrary-length parameter passing, but error_at has at most 7 extra parameters,
so it is feasible.
*/
void error_at(location_t loc, const char *gmsgid, ...) {
  static constexpr const char *keywords[] = {
      "-fsanitize=address",
      "incompatible",
      "-fsanitize=thread",
  };
  static constexpr const char *fallback_keywords[] = {
      "-fsanitize=%s",
      "incompatible",
  };
  static bool isXsanEnabled = __xsan::isXsanEnabled();
  llvm::StringRef msg(gmsgid);
  auto contains = [&](const char *kw) { return msg.contains(kw); };

  if (isXsanEnabled && llvm::all_of(keywords, contains)) {
    // When XSan is turned on, prevent errors from the front end
    // E.g., -fsanitize=address,thread will not raise any error.
    return;
  }

  va_list args;

  va_start(args, gmsgid);
  if (isXsanEnabled && llvm::all_of(fallback_keywords, contains) &&
      2 == count_gcc_diag_specifiers(gmsgid, "s")) {
    // For GCC-12+
    va_list args_copy;
    va_copy(args_copy, args);

    //  Get the first argument
    const char *san1 = va_arg(args, const char *);
    assert(san1 && "sanitizer name is null");
    const char *san2 = va_arg(args, const char *);
    assert(san2 && "sanitizer name is null");

    static constexpr llvm::StringRef masked_sanitizers[] = {"thread",
                                                            "address"};
    if (llvm::any_of(masked_sanitizers, [&](llvm::StringRef sanitizer) {
          return sanitizer == san1 || sanitizer == san2;
        })) {
      va_end(args);
      va_end(args_copy);
      return;
    }
  }

  // ------- Forward the variadic parameters ---------

  /*
  I counted all usage cases of error_at in gcc-12, and the variable-length
  parameter call usage is as follows:
  Call error_at with
   - 0 arguments [times: 1202]
   - 1 arguments [times: 1128]
   - 2 arguments [times: 428]
   - 3 arguments [times: 87]
   - 4 arguments [times: 29]
   - 5 arguments [times: 12]
   - 6 arguments [times: 3]
   - 7 arguments [times: 2]
  To be more robust, we increase the parameter processing count to 20

  Additionally, we found that the specifiers supported by error_at don't
  support float (unlike printf, i.e., they are not completely standard
  specifiers)
  */
  static constexpr unsigned MAX_ARGS = 20;

  unsigned count_extra_args = count_gcc_diag_specifiers(gmsgid);
  // Under System V ABI, floating-point parameter passing requires special
  // xmm registers; while the first 6 integers use rdi, rsi, rdx, rcx, r8, r9
  // registers, with subsequent ones pushed onto the stack.
  // Since GCC's diagnostic doesn't support floating-point numbers, all
  // parameters we pass are integers (pointers are also integers), so we
  // don't need to worry about calling convention issues

  switch (count_extra_args) {
#define FORWARD(num_args)                                                      \
  case num_args:                                                               \
    call_interceptor(loc, gmsgid, args, std::make_index_sequence<num_args>{}); \
    break;
    FORWARD(0)
    FORWARD(1)
    FORWARD(2)
    FORWARD(3)
    FORWARD(4)
    FORWARD(5)
    FORWARD(6)
    FORWARD(7)
    FORWARD(8)
    FORWARD(9)
    FORWARD(10)
    FORWARD(11)
    FORWARD(12)
    FORWARD(13)
    FORWARD(14)
    FORWARD(15)
    FORWARD(16)
    FORWARD(17)
    FORWARD(18)
    FORWARD(19)
    FORWARD(20)
#undef FORWARD
  default:
    FATAL("Unsupported number of arguments: %d (max %d)", count_extra_args,
          MAX_ARGS);
  }

  va_end(args);
}

/*
 * GCC diagnostic format specifiers for x86_64 GNU/Linux (System V ABI)
 *
 * This table shows the format specifiers supported by GCC's internal
 * diag() (error_at) function, along with their corresponding C/C++ types
 * and sizes in va_list:
 *
 * | Format | Description                    | C/C++ Type        | Size |
 * |--------|--------------------------------|-------------------|------|
 * | %d     | Signed decimal integer        | int               | 4    |
 * | %i     | Same as %d                    | int               | 4    |
 * | %o     | Unsigned octal integer        | unsigned int      | 4    |
 * | %u     | Unsigned decimal integer      | unsigned int      | 4    |
 * | %x     | Unsigned hex integer (lower)  | unsigned int      | 4    |
 * | %c     | Character (promoted to int)   | int               | 4    |
 * | %%     | Literal % (no arg consumed)   | —                 | —    |
 * | %s     | C string                      | char *            | 8    |
 * | %p     | Pointer                       | void *            | 8    |
 * | %*     | Width/precision wildcard      | int               | 4    |
 * | %ld    | Signed long decimal           | long              | 8    |
 * | %li    | Same as %ld                   | long              | 8    |
 * | %lo    | Unsigned long octal           | unsigned long     | 8    |
 * | %lu    | Unsigned long decimal         | unsigned long     | 8    |
 * | %lx    | Unsigned long hex             | unsigned long     | 8    |
 * | %lld   | Signed long long decimal      | long long         | 8    |
 * | %lli   | Same as %lld                  | long long         | 8    |
 * | %llo   | Unsigned long long octal      | unsigned long long| 8    |
 * | %llu   | Unsigned long long decimal    | unsigned long long| 8    |
 * | %llx   | Unsigned long long hex        | unsigned long long| 8    |
 * | %wd    | Signed __gcc_host_wide_int__  | long long         | 8    |
 * | %wi    | Same as %wd                   | long long         | 8    |
 * | %wo    | Unsigned wide int octal       | unsigned long long| 8    |
 * | %wu    | Unsigned wide int decimal     | unsigned long long| 8    |
 * | %wx    | Unsigned wide int hex         | unsigned long long| 8    |
 * | %D     | Print tree decl node          | tree (pointer)    | 8    |
 * | %E     | Print tree expr node          | tree (pointer)    | 8    |
 * | %O     | Print obstack or similar      | tree (pointer)    | 8    |
 * | %P     | Print generic node (debug)    | tree (pointer)    | 8    |
 * | %T     | Print tree type node          | tree (pointer)    | 8    |
 * | %qD    | Print quoted tree decl        | tree (pointer)    | 8    |
 * | %qE    | Print quoted tree expr        | tree (pointer)    | 8    |
 * | %qO    | Print quoted obstack          | tree (pointer)    | 8    |
 * | %qP    | Print quoted generic node     | tree (pointer)    | 8    |
 * | %qT    | Print quoted tree type        | tree (pointer)    | 8    |
 * | %qc    | Print quoted character        | int               | 4    |
 * | %qs    | Print quoted string           | char *            | 8    |
 * | %qp    | Print quoted pointer          | void *            | 8    |
 * | %m     | strerror(errno) (no arg)      | —                 | —    |
 *
 * Notes:
 * - tree is a pointer type in GCC, so it takes pointer size (8 bytes)
 * - All integer types smaller than 8 bytes (char→int, short→int, int) are
 *   integer-promoted and stored in 8-byte registers/stack slots under System V
 *   ABI, but va_arg(args,int) still extracts as int (4 bytes)
 * - Floating point (%f, %e, %g, etc.) and %n are not accepted in diagnostic
 *   framework
 * - %% and %m do not correspond to any user arguments
 *
 * This allows you to quickly determine which type to extract from va_list
 * using va_arg(args, ...) and the sizeof of that type in C++.
 */

/**
 * @brief Parse C-style format string and calculate the number of specifiers
 * @param gmsgid Format string pointer
 * @param given_spec If not empty, only count the specifiers in this string
 * @return Number of specifiers in the format string
 * @details GCC's diagnostic format string accepts specifiers that are not
 * standard, it doesn't accept floating-point numbers and accepts many extended
 * specifiers. We can dynamically calculate the required number of specifiers
 * based on the number of specifiers. For details, you can refer the comments
 * in the end part of diagnostic.cpp
 *
 */
static unsigned count_gcc_diag_specifiers(const char *gmsgid,
                                          std::string_view given_spec) {
  // Supported conversion specifiers (excluding "%%" and "%m")
  static constexpr std::array<std::string_view, 39> supported_specifiers = {
      // multi-char must come first (longer prefixes)
      "llx", "llo", "llu", "lli", "lld", "llX", "llO", "llU", // if ever needed
      "wd",  "wi",  "wo",  "wu",  "wx",  "ld",  "li",  "lo",  "lu", "lx", "qD",
      "qE",  "qO",  "qP",  "qT",  "qc",  "qs",  "qp",  "D",   "E",  "O",  "P",
      "T",   "d",   "i",   "o",   "u",   "x",   "c",   "s",   "p",
  };

  unsigned count = 0;
  size_t len = std::strlen(gmsgid);

  for (size_t i = 0; i < len; ++i) {
    if (gmsgid[i] != '%')
      continue;
    // skip "%%"
    if (i + 1 < len && gmsgid[i + 1] == '%') {
      ++i;
      continue;
    }
    intptr_t a;

    // potential start of a specifier
    size_t cur = i + 1;

    // skip any flags (none used by error_at except 'q' prefix)
    bool has_q = false;
    if (cur < len && gmsgid[cur] == 'q') {
      has_q = true;
      ++cur;
    }

    // now try to match the longest supported specifier at this position
    auto check_specifier = [len, cur, gmsgid,
                            &i](std::string_view spec) -> bool {
      size_t slen = spec.size();
      if (cur + slen > len)
        return false;
      std::string_view str2cmp(gmsgid + cur, slen);

      bool matched = str2cmp == spec;
      if (matched) {
        // found one
        i = cur + slen - 1; // advance i to end of this spec
      }
      return matched;
    };
    bool matched = given_spec.empty()
                       ? llvm::any_of(supported_specifiers, check_specifier)
                       : check_specifier(given_spec);
    if (matched)
      count++;

    // special case: "%m" (no argument consumed)
    if (!matched && cur < len && gmsgid[cur] == 'm')
      i = cur;
    // else unknown specifier: skip it
  }

  return count;
}

/*
@brief A more sound interception schema for varargs function, with the core
being trampoline implantation and stack frame sharing

@details
A more sound interception schema for varargs function, with the core being
trampoline implantation and stack frame sharing:
1. Get the real error_at address through dlsym
2. Implant a trampoline in the real error_at to make it jump to our handler
3. Execute handler:
  1. Write assembly by hand to save context
  2. call interceptor:
     1. Execute allow operation, if allowed, return false
     2. If not allowed, restore interceptee's code and return true
  3. Decide whether to call interceptee based on return value
  4. If allowed, return directly; if not allowed, execute real error_at code:
     1. Restore context and save ra
     2. (Optional) Modify ra to PostCall's address
     3. jmp error_at
  5. PostCall:
     1. Re-implant trampoline
     2. Restore ra
     3. Return
*/
