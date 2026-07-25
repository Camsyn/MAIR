; REQUIRES: mair_stats
; RUN: mair-opt -stats --disable-output -passes=simplifygraph %S/simplifygraph.ll 2>&1 | FileCheck %s --check-prefix=SIMPLIFY
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,locationmerge %S/locationmerge.ll 2>&1 | FileCheck %s --check-prefix=LOCATION
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,cmm %S/common-ma-motion.ll 2>&1 | FileCheck %s --check-prefix=COMMON
; RUN: mair-opt -stats --disable-output -passes=liom %S/loop-invariant-observation-motion.ll 2>&1 | FileCheck %s --check-prefix=INVARIANT
; RUN: mair-opt -stats --disable-output -passes=lpoc %S/loop-range-aggregation.ll 2>&1 | FileCheck %s --check-prefix=PERIODIC
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,roe %S/redundant-observation-elimination.ll 2>&1 | FileCheck %s --check-prefix=REDUNDANT
; RUN: mair-opt -stats --disable-output -O1 %S/pass-stat.ll 2>&1 | FileCheck %s --check-prefix=O1

; SIMPLIFY: 8 mair-simplifygraph - Number of observations entering graph simplification
; SIMPLIFY: {{[1-9][0-9]*}} mair-simplifygraph - Number of observations merged by graph simplification
; LOCATION: 15 mair-locationmerge - Number of memory accesses entering location merging
; LOCATION: {{[1-9][0-9]*}} mair-locationmerge - Number of accesses merged by location merging
; COMMON: 30 mair-commonmamotion - Number of memory accesses entering common MA motion
; PERIODIC: {{[1-9][0-9]*}} mair-loop-periodic-combine - Number of accesses aggregated across loop iterations
; PERIODIC: 13 mair-loop-periodic-combine - Number of memory accesses entering loop periodic combination
; INVARIANT: 7 mair-loop-invariant-motion - Number of memory accesses entering loop-invariant motion
; INVARIANT: {{[1-9][0-9]*}} mair-loop-invariant-motion - Number of invariant accesses moved to loop preheaders
; REDUNDANT-DAG: 13 mair-redundant-observation-elimination - Number of redundant accesses eliminated
; REDUNDANT-DAG: 4 mair-redundant-observation-elimination - Number of intra-observation redundant accesses eliminated
; REDUNDANT-DAG: 9 mair-redundant-observation-elimination - Number of inter-observation redundant accesses eliminated
; REDUNDANT: 31 mair-redundant-observation-elimination - Number of memory accesses entering redundant observation elimination

; O1: 4 mair-commonmamotion - Number of memory accesses entering common MA motion
; O1: 4 mair-locationmerge - Number of memory accesses entering location merging
; O1: 2 mair-loop-invariant-motion - Number of memory accesses entering loop-invariant motion
; O1: 2 mair-loop-periodic-combine - Number of memory accesses entering loop periodic combination
; O1: 4 mair-redundant-observation-elimination - Number of memory accesses entering redundant observation elimination
; O1: 11 mair-simplifygraph - Number of observations entering graph simplification
