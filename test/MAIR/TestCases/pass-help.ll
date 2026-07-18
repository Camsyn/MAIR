; RUN: mair-opt --help 2>&1 | FileCheck %s --check-prefix=HELP

; HELP: MAIRGraph pass names for -passes:
; HELP: cmm{{[[:space:]]+}}- Common MA Motion
; HELP: liom{{[[:space:]]+}}- Loop Invariant Observation Motion
; HELP: lpoc{{[[:space:]]+}}- Loop Periodic Observation Combine
; HELP: locationmerge{{[[:space:]]+}}- Location Merge
; HELP: roe{{[[:space:]]+}}- Redundant Observation Elimination
; HELP: simplifygraph{{[[:space:]]+}}- Simplify Graph
; HELP-NOT: commonmamotion
; HELP-NOT: loop-invariant-observation-motion
; HELP-NOT: loop-periodic-observation-combine
; HELP-NOT: redundantobservationelimination
