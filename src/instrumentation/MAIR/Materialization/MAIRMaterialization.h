#pragma once

#include "MAIR/MAIR.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"

namespace mair {

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
class MAIRObservationBackend {
public:
  virtual ~MAIRObservationBackend() = default;

  virtual llvm::Error prepare(const MAIRMaterializationPlan &Plan) = 0;
  virtual void emit(const MaterializedProgramPoint &PP,
                    llvm::IRBuilderBase &Builder) = 0;
};

} // namespace mair
