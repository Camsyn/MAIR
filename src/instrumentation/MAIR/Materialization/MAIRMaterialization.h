#pragma once

#include "MAIR/MAIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"

namespace mair {

/// The observations a sanitizer lowers through MAIR, and the source accesses
/// MAIR now owns. A source is owned unless some observation that cannot be
/// lowered still carries it; that source falls back to native lowering.
struct MAIRObservationSelection {
  llvm::SmallPtrSet<const Observation *, 32> Targets;
  llvm::SmallPtrSet<const llvm::Instruction *, 32> HandledSources;
};

/// Select the observations of \p Graph that \p CanInstrument accepts. A
/// candidate is dropped when any of its sources also belongs to an
/// observation that falls back; its other sources then fall back too, so a
/// source is handled only if every observation carrying it is lowered.
/// \p ModeledSources are the
/// accesses the graph held before optimization. Under `-mair-stats` this
/// reports a `select` visit: how many observations were lowered, how many
/// fell back and why, and how many source accesses fell back.
MAIRObservationSelection selectMaterializableObservations(
    const MAIRGraph &Graph,
    const llvm::SmallPtrSetImpl<const llvm::Instruction *> &ModeledSources,
    llvm::function_ref<bool(const Observation &)> CanInstrument);

/// The source of \p Obs that runs first, the one every other source follows.
/// Sources are loads and stores, so instruction dominance is the order they
/// run in. Sources merged from different paths, such as the two arms of a
/// branch, may have no such source; then this is a source that no other
/// source strictly dominates, picked by the observation's source order alone.
/// Sanitizers report a merged check at this source. Call it before
/// instantiation, which splits blocks without updating \p DT.
llvm::Instruction *getFirstSource(const Observation &Obs,
                                  const llvm::DominatorTree &DT);

struct PlannedObservation {
  const Observation *Source = nullptr;
  AccessKind Kind = AccessKind::Read;
  AccessProperty Properties = AccessProperty::None;
  MemoryRegion Region;
};

/// One self-contained program point in a materialization plan. The source is
/// retained only as identity; apply and backends consume the copied recipes.
struct MaterializationUnit {
  const ProgramPoint *Source = nullptr;
  ProgramPointSite Site;
  ProgramPointGuard Guard;
  SmallVector<PlannedObservation, 2> Observations;
};

/// The read-only plan passed to a backend before LLVM IR is mutated.
class MAIRMaterializationPlan {
  SmallVector<MaterializationUnit, 8> Units;

  friend class MAIRGraph;

public:
  ArrayRef<MaterializationUnit> units() const { return Units; }
  void addUnit(MaterializationUnit Unit) { Units.push_back(std::move(Unit)); }
};

struct MaterializedObservation {
  const Observation *Source = nullptr;
  AccessKind Kind = AccessKind::Read;
  AccessProperty Properties = AccessProperty::None;
  llvm::Value *Address = nullptr;
  llvm::Value *AccessSize = nullptr;
  llvm::Value *Stride = nullptr;
  llvm::Value *TripCount = nullptr;
  llvm::Value *Mask = nullptr;
  uint64_t MaskElementSize = 0;

  bool isPeriodic() const { return Stride != nullptr; }
  bool isMasked() const { return Mask != nullptr; }
};

struct MaterializedProgramPoint {
  const ProgramPoint *Source = nullptr;
  ProgramPoint::SiteKind SiteKind = ProgramPoint::SiteKind::MemoryInst;
  bool Guarded = false;
  ArrayRef<MaterializedObservation> Observations;
};

struct MAIRInstantiationResult {
  unsigned SplitEdges = 0;
  unsigned GuardBranches = 0;
  unsigned LoweredProgramPoints = 0;
  bool CFGChanged = false;
};

/// Backend boundary for generic MAIR materialization. `prepare` must be
/// read-only and reject all recoverable constraints before committed apply.
/// All guards and observation operands are materialized before the first
/// `emit`. Emission may split CFG blocks, but must preserve pending insertion
/// instructions and materialized operands. MAIR does not reuse CFG analyses
/// after emission begins.
class MAIRObservationBackend {
public:
  virtual ~MAIRObservationBackend() = default;

  virtual llvm::Error prepare(const MAIRMaterializationPlan &Plan) = 0;
  virtual void emit(const MaterializedProgramPoint &PP,
                    llvm::IRBuilderBase &Builder) = 0;
};

} // namespace mair
