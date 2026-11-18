; MAIR optimizes the observation of a volatile access like any other unless
; -distinguish-volatile keeps volatile observations apart. The accesses
; themselves are never touched.
; RUN: mair-opt %s | FileCheck %s --check-prefix=RAW
; RUN: mair-opt -passes=roe %s | FileCheck %s --check-prefix=ROE
; RUN: mair-opt -distinguish-volatile -passes=roe %s \
; RUN:   | FileCheck %s --check-prefix=ROE-DISTINCT
; RUN: mair-opt -passes="simplifygraph,locationmerge" %s \
; RUN:   | FileCheck %s --check-prefix=MERGE
; RUN: mair-opt -distinguish-volatile -passes="simplifygraph,locationmerge" %s \
; RUN:   | FileCheck %s --check-prefix=MERGE-DISTINCT

define void @volatile_after_plain(ptr %p) {
entry:
  %plain = load i32, ptr %p, align 4
  %volatile = load volatile i32, ptr %p, align 4
  ret void
}

; ROE-LABEL: mair graph of fn @volatile_after_plain {
; ROE:         o: read {{(volatile )?}}ptr %p, 4
; ROE-NOT:     o: read
; ROE:       }
; ROE-DISTINCT-LABEL: mair graph of fn @volatile_after_plain {
; ROE-DISTINCT:         o: read ptr %p, 4
; ROE-DISTINCT:         o: read volatile ptr %p, 4
; ROE-DISTINCT:       }

define void @plain_after_volatile(ptr %p) {
entry:
  %volatile = load volatile i32, ptr %p, align 4
  %plain = load i32, ptr %p, align 4
  ret void
}

; ROE-LABEL: mair graph of fn @plain_after_volatile {
; ROE:         o: read {{(volatile )?}}ptr %p, 4
; ROE-NOT:     o: read
; ROE:       }
; ROE-DISTINCT-LABEL: mair graph of fn @plain_after_volatile {
; ROE-DISTINCT:         o: read volatile ptr %p, 4
; ROE-DISTINCT:         o: read ptr %p, 4
; ROE-DISTINCT:       }

; The write covers the read before it, as in TSan's read-before-write rule.
define void @read_before_write(ptr %p) {
entry:
  %old = load volatile i32, ptr %p, align 4
  %new = add i32 %old, 1
  store volatile i32 %new, ptr %p, align 4
  ret void
}

; ROE-LABEL: mair graph of fn @read_before_write {
; ROE-NOT:     o: read
; ROE:         o: write volatile ptr %p, 4
; ROE:       }
; ROE-DISTINCT-LABEL: mair graph of fn @read_before_write {
; ROE-DISTINCT:         o: read volatile ptr %p, 4
; ROE-DISTINCT:         o: write volatile ptr %p, 4
; ROE-DISTINCT:       }

; The merged observation keeps the volatile property of its sources.
define void @adjacent(ptr %p) {
entry:
  %lo = load volatile i32, ptr %p, align 4
  %p4 = getelementptr i8, ptr %p, i64 4
  %hi = load i32, ptr %p4, align 4
  ret void
}

; MERGE-LABEL: mair graph of fn @adjacent {
; MERGE:         o: read volatile ptr %p, 8
; MERGE-NOT:     o: read
; MERGE:       }
; MERGE-DISTINCT-LABEL: mair graph of fn @adjacent {
; MERGE-DISTINCT-DAG:     o: read ptr %p + 4, 4
; MERGE-DISTINCT-DAG:     o: read volatile ptr %p, 4
; MERGE-DISTINCT:       }

define void @atomicrmw_properties(ptr %plain, ptr %vol) {
  %old = atomicrmw add ptr %plain, i32 1 monotonic
  %old_vol = atomicrmw volatile add ptr %vol, i32 1 monotonic
  ret void
}

; RAW-LABEL: mair graph of fn @atomicrmw_properties {
; RAW:         o: read/write atomic ptr %plain, 4
; RAW:         o: read/write volatile atomic ptr %vol, 4

define void @cmpxchg_properties(ptr %plain, ptr %vol) {
  %old = cmpxchg ptr %plain, i32 0, i32 1 monotonic monotonic
  %old_vol = cmpxchg volatile ptr %vol, i32 0, i32 1 monotonic monotonic
  ret void
}

; RAW-LABEL: mair graph of fn @cmpxchg_properties {
; RAW:         o: read/write atomic ptr %plain, 4
; RAW:         o: read/write volatile atomic ptr %vol, 4
