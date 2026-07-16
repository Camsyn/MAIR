; RUN: mair-opt -stats --disable-output -passes=simplifygraph %S/simplifygraph.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=SIMPLIFY < %t.stats
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,locationmerge %S/locationmerge.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=LOCATION < %t.stats
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,commonmamotion %S/common-ma-motion.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=COMMON < %t.stats
; RUN: mair-opt -stats --disable-output -passes=loop-invariant-observation-motion %S/loop-invariant-observation-motion.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=INVARIANT < %t.stats
; RUN: mair-opt -stats --disable-output -passes=loop-periodic-observation-combine %S/loop-range-aggregation.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=PERIODIC < %t.stats
; RUN: mair-opt -stats --disable-output -passes=redundantobservationelimination %S/redundant-observation-elimination.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=REDUNDANT < %t.stats
; RUN: mair-opt -stats --disable-output -O1 %S/pass-stat.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=O1 < %t.stats

; SIMPLIFY: 8 mair-simplifygraph - Number of observations entering graph simplification
; SIMPLIFY: {{[1-9][0-9]*}} mair-simplifygraph - Number of observations merged by graph simplification
; LOCATION: 13 mair-locationmerge - Number of memory accesses entering location merging
; LOCATION: {{[1-9][0-9]*}} mair-locationmerge - Number of accesses merged by location merging
; COMMON: {{[1-9][0-9]*}} mair-commonmamotion - Number of accesses hoisted by common MA motion
; COMMON: 30 mair-commonmamotion - Number of memory accesses entering common MA motion
; PERIODIC: {{[1-9][0-9]*}} mair-loop-periodic-combine - Number of accesses aggregated across loop iterations
; PERIODIC: 13 mair-loop-periodic-combine - Number of memory accesses entering loop periodic combination
; INVARIANT: 7 mair-loop-invariant-motion - Number of memory accesses entering loop-invariant motion
; INVARIANT: {{[1-9][0-9]*}} mair-loop-invariant-motion - Number of invariant accesses moved to loop preheaders
; REDUNDANT: {{[1-9][0-9]*}} mair-redundant-observation-elimination - Number of redundant accesses eliminated
; REDUNDANT: 31 mair-redundant-observation-elimination - Number of memory accesses entering redundant observation elimination

; O1: 4 mair-commonmamotion - Number of memory accesses entering common MA motion
; O1: 4 mair-locationmerge - Number of memory accesses entering location merging
; O1: 2 mair-loop-invariant-motion - Number of memory accesses entering loop-invariant motion
; O1: 2 mair-loop-periodic-combine - Number of memory accesses entering loop periodic combination
; O1: 4 mair-redundant-observation-elimination - Number of memory accesses entering redundant observation elimination
; O1: 11 mair-simplifygraph - Number of observations entering graph simplification
