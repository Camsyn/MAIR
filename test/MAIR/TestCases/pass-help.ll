; RUN: mair-opt --help 2>&1 | FileCheck %s --check-prefix=HELP \
; RUN:   --implicit-check-not="Color Options:" \
; RUN:   --implicit-check-not="General options:" \
; RUN:   --implicit-check-not="IR2Vec Options:" \
; RUN:   --implicit-check-not="--enable-vtable-profile-use"
; RUN: not mair-opt -passes=cmm --disable-output %S/common-observation-motion.ll 2>&1 | FileCheck %s --check-prefix=OLD-NAME

; OLD-NAME: unknown MAIRGraph pass 'cmm'

; HELP: MAIR Options:
; HELP: --O1{{[[:space:]]+}}- Run the standard MAIR O1 optimization pipeline
; HELP: --Og{{[[:space:]]+}}- Run the standard MAIR Og optimization pipeline
; HELP: --disable-output{{[[:space:]]+}}- Do not emit MAIR or LLVM output
; HELP: --emit-dot{{[[:space:]]+}}- Print MAIR graph as Graphviz DOT
; HELP: --emit-llvm{{[[:space:]]+}}- Print LLVM IR annotated with MAIR
; HELP: -o <filename>{{[[:space:]]+}}- Write output to <file>
; HELP: --passes=<pipeline>{{[[:space:]]+}}- MAIRGraph pass pipeline
; HELP: MAIRGraph pass names for -passes:
; HELP: com{{[[:space:]]+}}- Common Observation Motion
; HELP: liom{{[[:space:]]+}}- Loop Invariant Observation Motion
; HELP: lpoc{{[[:space:]]+}}- Loop Periodic Observation Combine
; HELP: locationmerge{{[[:space:]]+}}- Location Merge
; HELP: roe{{[[:space:]]+}}- Redundant Observation Elimination
; HELP: simplifygraph{{[[:space:]]+}}- Simplify Graph
; HELP: print<stat>{{[[:space:]]+}}- Print MAIR Statistics
; HELP-NOT: commonmamotion
; HELP-NOT: loop-invariant-observation-motion
; HELP-NOT: loop-periodic-observation-combine
; HELP-NOT: redundantobservationelimination
