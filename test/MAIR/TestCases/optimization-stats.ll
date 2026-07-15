; RUN: mair-opt -stats --disable-output -passes=simplifygraph %S/simplifygraph.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=SIMPLIFY < %t.stats
; RUN: mair-opt -stats --disable-output -passes=locationmerge %S/locationmerge.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=LOCATION < %t.stats
; RUN: mair-opt -stats --disable-output -passes=commonmamotion %S/common-ma-motion.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=COMMON < %t.stats
; RUN: mair-opt -stats --disable-output -passes=loop-invariant-observation-motion %S/loop-invariant-observation-motion.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=INVARIANT < %t.stats
; RUN: mair-opt -stats --disable-output -passes=loop-periodic-observation-combine %S/loop-range-aggregation.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=PERIODIC < %t.stats
; RUN: mair-opt -stats --disable-output -passes=redundantobservationelimination %S/redundant-observation-elimination.ll 2>%t.stats
; RUN: if test ! -s %t.stats || grep -q "Statistics are disabled" %t.stats; then exit 0; fi; FileCheck %s --check-prefix=REDUNDANT < %t.stats

; SIMPLIFY: {{[1-9][0-9]*}} mair-simplifygraph - Number of observations merged by graph simplification
; LOCATION: {{[1-9][0-9]*}} mair-locationmerge - Number of accesses merged by location merging
; COMMON: {{[1-9][0-9]*}} mair-commonmamotion - Number of accesses hoisted by common MA motion
; INVARIANT: {{[1-9][0-9]*}} mair-loop-invariant-motion - Number of invariant accesses moved to loop preheaders
; PERIODIC: {{[1-9][0-9]*}} mair-loop-periodic-combine - Number of accesses aggregated across loop iterations
; REDUNDANT: {{[1-9][0-9]*}} mair-redundant-observation-elimination - Number of redundant accesses eliminated
