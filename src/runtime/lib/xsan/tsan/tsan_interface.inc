//===-- tsan_interface.inc --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//

#include "../xsan_hooks_types.h"
#include "sanitizer_common/sanitizer_ptrauth.h"
#include "tsan_interface.h"
#include "tsan_rtl.h"
#include "tsan_rtl_extra.h"

#define CALLERPC ((uptr)__builtin_return_address(0))

using namespace __tsan;

void __tsan_read1(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 1,
               kAccessRead);
}

void __tsan_read2(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 2,
               kAccessRead);
}

void __tsan_read4(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 4,
               kAccessRead);
}

void __tsan_read8(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 8,
               kAccessRead);
}

void __tsan_read16(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess16(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                 kAccessRead);
}

void __tsan_write1(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 1,
               kAccessWrite);
}

void __tsan_write2(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 2,
               kAccessWrite);
}

void __tsan_write4(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 4,
               kAccessWrite);
}

void __tsan_write8(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, 8,
               kAccessWrite);
}

void __tsan_write16(void *addr) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess16(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                 kAccessWrite);
}

void __tsan_read1_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 1,
               kAccessRead | kAccessExternalPC);
}

void __tsan_read2_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 2,
               kAccessRead | kAccessExternalPC);
}

void __tsan_read4_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 4,
               kAccessRead | kAccessExternalPC);
}

void __tsan_read8_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 8,
               kAccessRead | kAccessExternalPC);
}

void __tsan_write1_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 1,
               kAccessWrite | kAccessExternalPC);
}

void __tsan_write2_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 2,
               kAccessWrite | kAccessExternalPC);
}

void __tsan_write4_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 4,
               kAccessWrite | kAccessExternalPC);
}

void __tsan_write8_pc(void *addr, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)), (uptr)addr, 8,
               kAccessWrite | kAccessExternalPC);
}

ALWAYS_INLINE USED void __tsan_unaligned_read2(const void *addr) {
  TSAN_CHECK_GUARD(addr)
  UnalignedMemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                        2, kAccessRead);
}

ALWAYS_INLINE USED void __tsan_unaligned_read4(const void *addr) {
  TSAN_CHECK_GUARD(addr)
  UnalignedMemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                        4, kAccessRead);
}

ALWAYS_INLINE USED void __tsan_unaligned_read8(const void *addr) {
  TSAN_CHECK_GUARD(addr)
  UnalignedMemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                        8, kAccessRead);
}

ALWAYS_INLINE USED void __tsan_unaligned_write2(void *addr) {
  TSAN_CHECK_GUARD(addr)
  UnalignedMemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                        2, kAccessWrite);
}

ALWAYS_INLINE USED void __tsan_unaligned_write4(void *addr) {
  TSAN_CHECK_GUARD(addr)
  UnalignedMemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                        4, kAccessWrite);
}

ALWAYS_INLINE USED void __tsan_unaligned_write8(void *addr) {
  TSAN_CHECK_GUARD(addr)
  UnalignedMemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr,
                        8, kAccessWrite);
}

extern "C" {
/// Delegated these interface implemantations to XSan.
// // __sanitizer_unaligned_load/store are for user instrumentation.
// SANITIZER_INTERFACE_ATTRIBUTE
// u16 __sanitizer_unaligned_load16(const uu16 *addr) {
//   __tsan_unaligned_read2(addr);
//   return *addr;
// }

// SANITIZER_INTERFACE_ATTRIBUTE
// u32 __sanitizer_unaligned_load32(const uu32 *addr) {
//   __tsan_unaligned_read4(addr);
//   return *addr;
// }

// SANITIZER_INTERFACE_ATTRIBUTE
// u64 __sanitizer_unaligned_load64(const uu64 *addr) {
//   __tsan_unaligned_read8(addr);
//   return *addr;
// }

// SANITIZER_INTERFACE_ATTRIBUTE
// void __sanitizer_unaligned_store16(uu16 *addr, u16 v) {
//   *addr = v;
//   __tsan_unaligned_write2(addr);
// }

// SANITIZER_INTERFACE_ATTRIBUTE
// void __sanitizer_unaligned_store32(uu32 *addr, u32 v) {
//   *addr = v;
//   __tsan_unaligned_write4(addr);
// }

// SANITIZER_INTERFACE_ATTRIBUTE
// void __sanitizer_unaligned_store64(uu64 *addr, u64 v) {
//   *addr = v;
//   __tsan_unaligned_write8(addr);
// }
}

void __tsan_vptr_update(void **vptr_p, void *new_val) {
  if (*vptr_p == new_val)
    return;
  TSAN_CHECK_GUARD(vptr_p)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)vptr_p,
               sizeof(*vptr_p), kAccessWrite | kAccessVptr);
}

void __tsan_vptr_read(void **vptr_p) {
  TSAN_CHECK_GUARD(vptr_p)
  MemoryAccess(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)vptr_p,
               sizeof(*vptr_p), kAccessRead | kAccessVptr);
}

void __tsan_func_entry(void *pc) {
  FuncEntry(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)));
}

void __tsan_func_exit() { FuncExit(cur_thread()); }

void __tsan_ignore_thread_begin() {
  ThreadIgnoreBegin(cur_thread(), __xsan::GetRealPc(CALLERPC));
}

void __tsan_ignore_thread_end() { ThreadIgnoreEnd(cur_thread()); }

void __tsan_read_range(void *addr, uptr size) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccessRange(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, size,
                    false);
}

void __tsan_write_range(void *addr, uptr size) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccessRange(cur_thread(), __xsan::GetRealPc(CALLERPC), (uptr)addr, size,
                    true);
}

void __tsan_read_range_pc(void *addr, uptr size, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccessRange(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)),
                    (uptr)addr, size, false);
}

void __tsan_write_range_pc(void *addr, uptr size, void *pc) {
  TSAN_CHECK_GUARD(addr)
  MemoryAccessRange(cur_thread(), __xsan::GetRealPc(STRIP_PAC_PC(pc)),
                    (uptr)addr, size, true);
}
