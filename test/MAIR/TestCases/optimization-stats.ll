; REQUIRES: mair_stats
; RUN: mair-opt -stats --disable-output -passes=simplifygraph %S/simplifygraph.ll 2>&1 | FileCheck %s --check-prefix=SIMPLIFY
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,locationmerge %S/locationmerge.ll 2>&1 | FileCheck %s --check-prefix=LOCATION
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,com %S/common-observation-motion.ll 2>&1 | FileCheck %s --check-prefix=COMMON
; RUN: mair-opt -stats --disable-output -passes=liom %S/loop-invariant-observation-motion.ll 2>&1 | FileCheck %s --check-prefix=INVARIANT
; RUN: mair-opt -stats --disable-output -passes=lpoc %S/loop-range-aggregation.ll 2>&1 | FileCheck %s --check-prefix=PERIODIC
; RUN: mair-opt -stats --disable-output -passes=simplifygraph,roe %S/redundant-observation-elimination.ll 2>&1 | FileCheck %s --check-prefix=REDUNDANT
; RUN: mair-opt -stats --disable-output -O1 %S/pass-stat.ll 2>&1 | FileCheck %s --check-prefix=O1

; SIMPLIFY: 8 mair-simplifygraph - Number of program points entering graph simplification
; SIMPLIFY: {{[1-9][0-9]*}} mair-simplifygraph - Number of program points merged by graph simplification
; LOCATION: 15 mair-locationmerge - Number of observations entering location merging
; LOCATION: {{[1-9][0-9]*}} mair-locationmerge - Number of observations merged by location merging
; COMMON: 30 mair-common-observation-motion - Number of observations entering common observation motion
; PERIODIC: {{[1-9][0-9]*}} mair-loop-periodic-combine - Number of observations aggregated across loop iterations
; PERIODIC: 13 mair-loop-periodic-combine - Number of observations entering loop periodic combination
; INVARIANT: 7 mair-loop-invariant-motion - Number of observations entering loop-invariant motion
; INVARIANT: {{[1-9][0-9]*}} mair-loop-invariant-motion - Number of invariant observations moved to loop preheaders
; REDUNDANT-DAG: 13 mair-redundant-observation-elimination - Number of redundant observations eliminated
; REDUNDANT-DAG: 4 mair-redundant-observation-elimination - Number of intra-program-point redundant observations eliminated
; REDUNDANT-DAG: 9 mair-redundant-observation-elimination - Number of inter-program-point redundant observations eliminated
; REDUNDANT: 31 mair-redundant-observation-elimination - Number of observations entering redundant observation elimination

; O1: 2 mair-common-observation-motion - Number of observations entering common observation motion
; O1: 4 mair-locationmerge - Number of observations entering location merging
; O1: 2 mair-loop-invariant-motion - Number of observations entering loop-invariant motion
; O1: 2 mair-loop-periodic-combine - Number of observations entering loop periodic combination
; O1: 4 mair-redundant-observation-elimination - Number of observations entering redundant observation elimination
; O1: 11 mair-simplifygraph - Number of program points entering graph simplification
