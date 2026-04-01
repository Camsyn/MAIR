#include <sanitizer_common/sanitizer_platform.h>
#if SANITIZER_FREEBSD || SANITIZER_LINUX || SANITIZER_NETBSD || \
    SANITIZER_SOLARIS

#  include <dlfcn.h>
#  include <fcntl.h>
#  include <limits.h>
#  include <pthread.h>
#  include <stdio.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#  include <sys/syscall.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <unwind.h>

#  include "asan/asan_allocator.h"
#  include "sanitizer_common/sanitizer_flags.h"
#  include "sanitizer_common/sanitizer_hash.h"
#  include "sanitizer_common/sanitizer_libc.h"
#  include "sanitizer_common/sanitizer_procmaps.h"
#  include "xsan_interceptors.h"
#  include "xsan_internal.h"
#  include "xsan_platform.h"

#  if SANITIZER_FREEBSD
#    include <sys/link_elf.h>
#  endif
#  if SANITIZER_LINUX
#    include <setjmp.h>
#    include <sys/personality.h>
#  endif
#  if SANITIZER_SOLARIS
#    include <link.h>
#  endif

#  if SANITIZER_ANDROID || SANITIZER_FREEBSD || SANITIZER_SOLARIS
#    include <ucontext.h>
#  elif SANITIZER_NETBSD
#    include <link_elf.h>
#    include <ucontext.h>
#  else
#    include <link.h>
#    include <sys/ucontext.h>
#  endif

typedef enum {
  XSAN_RT_VERSION_UNDEFINED = 0,
  XSAN_RT_VERSION_DYNAMIC,
  XSAN_RT_VERSION_STATIC,
} xsan_rt_version_t;

// FIXME: perhaps also store abi version here?
extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE
xsan_rt_version_t __xsan_rt_version;
}

namespace __xsan {

void InitializePlatformInterceptors() {}
void InitializePlatformExceptionHandlers() {}
bool IsSystemHeapAddress(uptr addr) { return false; }

#  if XSAN_PREMAP_SHADOW
uptr FindPremappedShadowStart(uptr shadow_size_bytes) {
  uptr granularity = GetMmapGranularity();
  uptr shadow_start = reinterpret_cast<uptr>(&__xsan_shadow);
  uptr premap_shadow_size = PremapShadowSize();
  uptr shadow_size = RoundUpTo(shadow_size_bytes, granularity);
  // We may have mapped too much. Release extra memory.
  UnmapFromTo(shadow_start + shadow_size, shadow_start + premap_shadow_size);
  return shadow_start;
}
#  endif

// uptr FindDynamicShadowStart() {
//   uptr shadow_size_bytes = MemToShadowSize(kHighMemEnd);
// #  if XSAN_PREMAP_SHADOW
//   if (!PremapShadowFailed())
//     return FindPremappedShadowStart(shadow_size_bytes);
// #  endif

//   return MapDynamicShadow(shadow_size_bytes, XSAN_SHADOW_SCALE,
//                           /*min_shadow_base_alignment*/ 0, kHighMemEnd,
//                           GetMmapGranularity());
// }

// void XsanApplyToGlobals(globals_op_fptr op, const void *needle) {
//   UNIMPLEMENTED();
// }

// void FlushUnneededXSanShadowMemory(uptr p, uptr size) {
//   // Since xsan's mapping is compacting, the shadow chunk may be
//   // not page-aligned, so we only flush the page-aligned portion.
//   ReleaseMemoryPagesToOS(MemToShadow(p), MemToShadow(p + size));
// }

#  if SANITIZER_ANDROID
// FIXME: should we do anything for Android?
void XsanCheckDynamicRTPrereqs() {}
void XsanCheckIncompatibleRT() {}
#  else
static int FindFirstDSOCallback(struct dl_phdr_info *info, size_t size,
                                void *data) {
  VReport(2, "info->dlpi_name = %s\tinfo->dlpi_addr = %p\n", info->dlpi_name,
          (void *)info->dlpi_addr);

  const char **name = (const char **)data;

  // Ignore first entry (the main program)
  if (!*name) {
    *name = "";
    return 0;
  }

#    if SANITIZER_LINUX
  // Ignore vDSO. glibc versions earlier than 2.15 (and some patched
  // by distributors) return an empty name for the vDSO entry, so
  // detect this as well.
  if (!info->dlpi_name[0] ||
      internal_strncmp(info->dlpi_name, "linux-", sizeof("linux-") - 1) == 0)
    return 0;
#    endif
#    if SANITIZER_FREEBSD
  // Ignore vDSO.
  if (internal_strcmp(info->dlpi_name, "[vdso]") == 0)
    return 0;
#    endif

  *name = info->dlpi_name;
  return 1;
}

static bool IsDynamicRTName(const char *libname) {
  return internal_strstr(libname, "libclang_rt.xsan") ||
         internal_strstr(libname, "libxsan.so");
}

static void ReportIncompatibleRT() {
  Report("Your application is linked against incompatible XSan runtimes.\n");
  Die();
}

void XsanCheckDynamicRTPrereqs() {
  if (!XSAN_DYNAMIC || !flags()->verify_xsan_link_order)
    return;

  // Ensure that dynamic RT is the first DSO in the list
  const char *first_dso_name = nullptr;
  dl_iterate_phdr(FindFirstDSOCallback, &first_dso_name);
  if (first_dso_name && first_dso_name[0] && !IsDynamicRTName(first_dso_name)) {
    Report(
        "XSan runtime does not come first in initial library list; "
        "you should either link runtime to your application or "
        "manually preload it with LD_PRELOAD.\n");
    Die();
  }
}

void XsanCheckIncompatibleRT() {
  if (XSAN_DYNAMIC) {
    if (__xsan_rt_version == XSAN_RT_VERSION_UNDEFINED) {
      __xsan_rt_version = XSAN_RT_VERSION_DYNAMIC;
    } else if (__xsan_rt_version != XSAN_RT_VERSION_DYNAMIC) {
      ReportIncompatibleRT();
    }
  } else {
    if (__xsan_rt_version == XSAN_RT_VERSION_UNDEFINED) {
      // Ensure that dynamic runtime is not present. We should detect it
      // as early as possible, otherwise XSan interceptors could bind to
      // the functions in dynamic XSan runtime instead of the functions in
      // system libraries, causing crashes later in XSan initialization.
      MemoryMappingLayout proc_maps(/*cache_enabled*/ true);
      char filename[PATH_MAX];
      MemoryMappedSegment segment(filename, sizeof(filename));
      while (proc_maps.Next(&segment)) {
        if (IsDynamicRTName(segment.filename)) {
          ReportIncompatibleRT();
        }
      }
      __xsan_rt_version = XSAN_RT_VERSION_STATIC;
    } else if (__xsan_rt_version != XSAN_RT_VERSION_STATIC) {
      ReportIncompatibleRT();
    }
  }
}
#  endif  // SANITIZER_ANDROID

#  if XSAN_INTERCEPT_SWAPCONTEXT
constexpr u32 kXsanContextStackFlagsMagic = 0x51260eea;

static int HashContextStack(const ucontext_t &ucp) {
  MurMur2Hash64Builder hash(kXsanContextStackFlagsMagic);
  hash.add(reinterpret_cast<uptr>(ucp.uc_stack.ss_sp));
  hash.add(ucp.uc_stack.ss_size);
  return static_cast<int>(hash.get());
}

void SignContextStack(void *context) {
  ucontext_t *ucp = reinterpret_cast<ucontext_t *>(context);
  ucp->uc_stack.ss_flags = HashContextStack(*ucp);
}

void ReadContextStack(void *context, uptr *stack, uptr *ssize) {
  const ucontext_t *ucp = reinterpret_cast<const ucontext_t *>(context);
  if (HashContextStack(*ucp) == ucp->uc_stack.ss_flags) {
    *stack = reinterpret_cast<uptr>(ucp->uc_stack.ss_sp);
    *ssize = ucp->uc_stack.ss_size;
    return;
  }
  *stack = 0;
  *ssize = 0;
}
#  endif  // XSAN_INTERCEPT_SWAPCONTEXT

void *XsanDlSymNext(const char *sym) { return dlsym(RTLD_NEXT, sym); }

bool HandleDlopenInit() {
  // Not supported on this platform.
  static_assert(!SANITIZER_SUPPORTS_INIT_FOR_DLOPEN,
                "Expected SANITIZER_SUPPORTS_INIT_FOR_DLOPEN to be false");
  return false;
}

// With the zero shadow base we can not actually map pages starting from 0.
// This constant is somewhat arbitrary.
constexpr uptr ZeroBaseShadowStart = 0;
constexpr uptr ZeroBaseMaxShadowStart = 1 << 18;

/// TODO: decouple ASan's allocator from XSan
// NOTE: This type alias creates a direct dependency on ASan's internal
// allocator type. This introduces tight coupling and may make the code brittle
// to changes in ASan's internal structure. If ASan's internal allocator
// changes, this code may break and require updates.
using PrimaryAllocator = __asan::PrimaryAllocator;

uptr ALWAYS_INLINE HeapEnd() {
  return HeapMemEnd() + PrimaryAllocator::AdditionalSize();
}

static void ProtectRange(uptr beg, uptr end) {
  if (beg == end)
    return;
  ProtectGap(beg, end - beg, ZeroBaseShadowStart, ZeroBaseMaxShadowStart);
}

/// Come from tsan_platform_posix.cpp
// CheckAndProtect will check if the memory layout is compatible with XSan.
// Optionally (if 'protect' is true), it will set the memory regions between
// app memory to be inaccessible.
// 'ignore_heap' means it will not consider heap memory allocations to be a
// conflict. Set this based on whether we are calling CheckAndProtect before
// or after the allocator has initialized the heap.
/// @param protect If true, protect the memory.
/// @param ignore_heap If true, ignore the heap memory.
/// @param print_warnings If true, print warnings about conflicting memory
/// mappings.
static bool CheckAndProtect(bool protect, bool ignore_heap,
                            bool print_warnings) {
  // Ensure that the binary is indeed compiled with -pie.
  MemoryMappingLayout proc_maps(true);
  MemoryMappedSegment segment;
  while (proc_maps.Next(&segment)) {
    if (segment.start >= HeapMemBeg() && segment.end <= HeapEnd()) {
      if (ignore_heap)
        continue;
      else
        return false;
    }

    // Note: IsAppMem includes if it is heap memory, hence we must
    // put this check after the heap bounds check.
    if (IsAppMem(segment.start))
      continue;

    // Guard page after the heap end
    if (segment.start >= HeapMemEnd() && segment.start < HeapEnd())
      continue;
    if (segment.protection == 0)  // Zero page or mprotected.
      continue;
    if (segment.start >= VdsoBeg())  // vdso
      break;

    if (print_warnings)
      Printf("WARNING: XSan: unexpected memory mapping 0x%zx-0x%zx\n",
             segment.start, segment.end);

    return false;
  }

  if (!protect)
    return true;

#  if SANITIZER_IOS && !SANITIZER_IOSSIM
  ProtectRange(HeapMemEnd(), TsanShadowBeg());
  ProtectRange(TsanShadowEnd(), TsanMetaShadowBeg());
  ProtectRange(TsanMetaShadowEnd(), HiAppMemBeg());
#  else
  SmallVector<NamedRange, 1024> map_ranges;

  /*
   * Currently, [lo_app, lo_end) overlaps with ASan's shadow
   *
   * NOTE: Special handling for LoApp region due to ASan shadow mapping.
   *
   * - ASan's shadow offset is difficult to change (tied to ABI and intrinsics),
   * so its LoAppMemEnd() is much smaller than what is needed for other
   * sanitizers. For example, ideally, LoAppMemEnd() should at least cover the
   * range 0x003000000000 - 0x005000000000 (the "prelink" segment).
   *
   * - To address this, we provide a dedicated AsanLoAppMemEnd() for ASan, while
   *   other sanitizers use a larger LoAppMemEnd(). This may result in LoApp's
   * end address overlapping with ASan's shadow region:
   *
   *       LoAppEnd ∈ [AsanShadowBeg, AsanShadowEnd)
   *
   * - This overlap is safe: ASan itself ensures that any overlapped memory is
   *   inaccessible to the application, preventing incorrect access.
   *
   * - In CheckAndProtect, we deliberately skip (ignore) protection/mapping for
   *   LoApp. This works because the range from LoAppMemBeg() to AsanShadowEnd()
   *   is continuous (with no other gaps that require protection by XSan).
   *
   * - Therefore, real LoApp is not included in the protection/mapping flows
   *   here. We instead use AsanLoAppMemEnd() to represent the end of the low
   *   app memory.
   */
#    if !XSAN_CONTAINS_ASAN
  map_ranges.push_back({{LoAppMemBeg(), LoAppMemEnd()}, "low app memory"});
#    else
  map_ranges.push_back(
      {{LoAppMemBeg(), AsanLoAppMemEnd()}, "low app memory (for ASan)"});
#    endif

  map_ranges.push_back({{MidAppMemBeg(), MidAppMemEnd()}, "mid app memory"});
  map_ranges.push_back({{HeapMemBeg(), HeapMemEnd()}, "heap memory"});
  map_ranges.push_back({{HiAppMemBeg(), HiAppMemEnd()}, "high app memory"});
  NeededMapRanges(map_ranges);
  Sort(map_ranges.data, map_ranges.size(),
       [](const NamedRange &lh, const NamedRange &rh) {
         return lh.range.begin < rh.range.begin;
       });

  NamedRange fake_range = {{0, 0}, "fake range"};
  const NamedRange *last_range = &fake_range;
  for (const auto &cur_range : map_ranges) {
    VPrintf(1, "Protecting gap before: %s: 0x%zx-0x%zx\n", cur_range.name,
            cur_range.range.begin, cur_range.range.end);
    if (cur_range.range.begin < last_range->range.end) {
      Report(
          "FATAL: XSan: overlapping memory mapping between:\n"
          "%s: \t0x%zx-0x%zx\n"
          "%s: \t0x%zx-0x%zx\n",
          last_range->name, last_range->range.begin, last_range->range.end,
          cur_range.name, cur_range.range.begin, cur_range.range.end);
      Die();
    }
    ProtectRange(last_range->range.end, cur_range.range.begin);
    last_range = &cur_range;
  }

#  endif

#  if defined(__s390x__)
  // Protect the rest of the address space.
  const uptr user_addr_max_l4 = 0x0020000000000000ull;
  const uptr user_addr_max_l5 = 0xfffffffffffff000ull;
  // All the maintained s390x kernels support at least 4-level page tables.
  ProtectRange(HiAppMemEnd(), user_addr_max_l4);
  // Older s390x kernels may not support 5-level page tables.
  TryProtectRange(user_addr_max_l4, user_addr_max_l5);
#  endif
  return true;
}

#  if !SANITIZER_GO
/// Modified from tsan_platform_linux.cpp
static void ReExecIfNeeded(bool ignore_heap) {
  // Go maps shadow memory lazily and works fine with limited address space.
  // Unlimited stack is not a problem as well, because the executable
  // is not compiled with -pie.
  bool reexec = false;
  // TSan doesn't play well with unlimited stack size (as stack
  // overlaps with shadow memory). If we detect unlimited stack size,
  // we re-exec the program with limited stack size as a best effort.
  if (StackSizeIsUnlimited()) {
    const uptr kMaxStackSize = 32 * 1024 * 1024;
    VReport(1,
            "Program is run with unlimited stack size, which wouldn't "
            "work with ThreadSanitizer.\n"
            "Re-execing with stack size limited to %zd bytes.\n",
            kMaxStackSize);
    SetStackSizeLimitInBytes(kMaxStackSize);
    reexec = true;
  }

  if (!AddressSpaceIsUnlimited()) {
    Report(
        "WARNING: Program is run with limited virtual address space,"
        " which wouldn't work with ThreadSanitizer.\n");
    Report("Re-execing with unlimited virtual address space.\n");
    SetAddressSpaceUnlimited();
    reexec = true;
  }

#    if SANITIZER_LINUX
#      if SANITIZER_ANDROID && (defined(__aarch64__) || defined(__x86_64__))
  // ASLR personality check.
  int old_personality = personality(0xffffffff);
  bool aslr_on =
      (old_personality != -1) && ((old_personality & ADDR_NO_RANDOMIZE) == 0);

  // After patch "arm64: mm: support ARCH_MMAP_RND_BITS." is introduced in
  // linux kernel, the random gap between stack and mapped area is increased
  // from 128M to 36G on 39-bit aarch64. As it is almost impossible to cover
  // this big range, we should disable randomized virtual space on aarch64.
  if (aslr_on) {
    VReport(1,
            "WARNING: Program is run with randomized virtual address "
            "space, which wouldn't work with ThreadSanitizer on Android.\n"
            "Re-execing with fixed virtual address space.\n");
    CHECK_NE(personality(old_personality | ADDR_NO_RANDOMIZE), -1);
    reexec = true;
  }
#      endif

  if (reexec) {
    // Don't check the address space since we're going to re-exec anyway.
  } else if (!CheckAndProtect(false, ignore_heap, false)) {
    // ASLR personality check.
    // N.B. 'personality' is sometimes forbidden by sandboxes, so we only call
    // this as a last resort (when the memory mapping is incompatible and TSan
    // would fail anyway).
    int old_personality = personality(0xffffffff);
    bool aslr_on =
        (old_personality != -1) && ((old_personality & ADDR_NO_RANDOMIZE) == 0);
    if (aslr_on) {
      // Disable ASLR if the memory layout was incompatible.
      // Alternatively, we could just keep re-execing until we get lucky
      // with a compatible randomized layout, but the risk is that if it's
      // not an ASLR-related issue, we will be stuck in an infinite loop of
      // re-execing (unless we change ReExec to pass a parameter of the
      // number of retries allowed.)
      VReport(1,
              "WARNING: XSan: memory layout is incompatible, "
              "possibly due to high-entropy ASLR.\n"
              "Re-execing with fixed virtual address space.\n"
              "N.B. reducing ASLR entropy is preferable.\n");
      CHECK_NE(personality(old_personality | ADDR_NO_RANDOMIZE), -1);
      reexec = true;
    } else {
      Printf(
          "FATAL: XSan: memory layout is incompatible, "
          "even though ASLR is disabled.\n"
          "Please file a bug.\n");
      DumpProcessMap();
      Die();
    }
  }
#    endif  // SANITIZER_LINUX

  if (reexec)
    ReExec();
}
#  endif  //! SANITIZER_GO

/// Modified from tsan_platform_linux.cpp
void InitializePlatformEarly() {
  vmaSize = (MostSignificantSetBitIndex(GET_CURRENT_FRAME()) + 1);
#  if defined(__aarch64__)
#    if !SANITIZER_GO
  if (vmaSize != 39 && vmaSize != 42 && vmaSize != 48) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 39, 42 and 48\n", vmaSize);
    Die();
  }
#    else
  if (vmaSize != 48) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 48\n", vmaSize);
    Die();
  }
#    endif
#  elif SANITIZER_LOONGARCH64
#    if !SANITIZER_GO
  if (vmaSize != 47) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 47\n", vmaSize);
    Die();
  }
#    else
  if (vmaSize != 47) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 47\n", vmaSize);
    Die();
  }
#    endif
#  elif defined(__powerpc64__)
#    if !SANITIZER_GO
  if (vmaSize != 44 && vmaSize != 46 && vmaSize != 47) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 44, 46, and 47\n", vmaSize);
    Die();
  }
#    else
  if (vmaSize != 46 && vmaSize != 47) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 46, and 47\n", vmaSize);
    Die();
  }
#    endif
#  elif defined(__mips64)
#    if !SANITIZER_GO
  if (vmaSize != 40) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 40\n", vmaSize);
    Die();
  }
#    else
  if (vmaSize != 47) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 47\n", vmaSize);
    Die();
  }
#    endif
#  elif SANITIZER_RISCV64
  // the bottom half of vma is allocated for userspace
  vmaSize = vmaSize + 1;
#    if !SANITIZER_GO
  if (vmaSize != 39 && vmaSize != 48) {
    Printf("FATAL: Xsan: unsupported VMA range\n");
    Printf("FATAL: Found %zd - Supported 39 and 48\n", vmaSize);
    Die();
  }
#    endif
#  endif

#  if !SANITIZER_GO
  /// We need ReRun() without ASLR if the memory layout does not fit
  /// xsan_platform.h.
  /// Although the following commit said this might "call `getlim` before
  /// interceptors are initialized.", we still need to find another way to
  /// tackle that problem.
  /// https://github.com/Camsyn/XSan/commit/eb6395ee8f12d8b91d50f0b88b614c6ed76c1ab2
  // Heap has not been allocated yet
  ReExecIfNeeded(false);
#  endif
}

/// Modified from tsan_platform_linux.cpp
/// Core: if memory mappings does not fit xsan_platform.h, ReExec() is called.
void InitializePlatform() {
  DisableCoreDumperIfNecessary();
  // Go maps shadow memory lazily and works fine with limited address space.
  // Unlimited stack is not a problem as well, because the executable
  // is not compiled with -pie.
#  if !SANITIZER_GO
// {
// #    if SANITIZER_LINUX && (defined(__aarch64__) || defined(__loongarch_lp64))
//    // Initialize the xor key used in {sig}{set,long}jump.
//    InitializeLongjmpXorKey();
// #    endif
// }

  // We called ReExecIfNeeded() in InitializePlatformEarly(), but there are
  // intervening allocations that result in an edge case:
  // 1) InitializePlatformEarly(): memory layout is compatible
  // 2) Intervening allocations happen
  // 3) InitializePlatform(): memory layout is incompatible and fails
  //    CheckAndProtect()
#    if !SANITIZER_GO
  // Heap has already been allocated
  ReExecIfNeeded(!is_heap_init);
#    endif

  // Earlier initialization steps already re-exec'ed until we got a compatible
  // memory layout, so we don't expect any more issues here.
  if (!CheckAndProtect(true, !is_heap_init, true)) {
    Printf(
        "FATAL: XSan: unexpectedly found incompatible memory "
        "layout.\n");
    Printf("FATAL: Please file a bug.\n");
    DumpProcessMap();
    Die();
  }

#  endif  // !SANITIZER_GO
}

}  // namespace __xsan

#endif  // SANITIZER_FREEBSD || SANITIZER_LINUX || SANITIZER_NETBSD ||
        // SANITIZER_SOLARIS
