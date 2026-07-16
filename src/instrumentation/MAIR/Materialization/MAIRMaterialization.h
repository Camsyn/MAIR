#pragma once

#include "MAIR/MAIR.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"

namespace mair {

struct PlannedAccess {
  const MemoryAccess *Source = nullptr;
  AccessKind Kind = AccessKind::Read;
  AccessProperty Properties = AccessProperty::None;
  MemoryRegion Region;
};

/// One self-contained semantic event in a materialization plan. The source is
/// retained only as identity; apply and backends consume the copied recipes.
struct MaterializationUnit {
  const Observation *Source = nullptr;
  ObservationSite Site;
  ObservationGuard Guard;
  SmallVector<PlannedAccess, 2> Accesses;
};

/// The read-only plan passed to a backend before LLVM IR is mutated.
class MAIRMaterializationPlan {
  SmallVector<MaterializationUnit, 8> Units;

  friend class MAIRGraph;

public:
  ArrayRef<MaterializationUnit> units() const { return Units; }
  void addUnit(MaterializationUnit Unit) { Units.push_back(std::move(Unit)); }
};

struct MaterializedAccess {
  const MemoryAccess *Source = nullptr;
  AccessKind Kind = AccessKind::Read;
  AccessProperty Properties = AccessProperty::None;
  llvm::Value *Address = nullptr;
  llvm::Value *AccessSize = nullptr;
  llvm::Value *Stride = nullptr;
  llvm::Value *TripCount = nullptr;

  bool isPeriodic() const { return Stride != nullptr; }
};

struct MaterializedObservation {
  const Observation *Source = nullptr;
  Observation::SiteKind SiteKind = Observation::SiteKind::MemoryInst;
  bool Guarded = false;
  ArrayRef<MaterializedAccess> Accesses;
};

struct MAIRInstantiationResult {
  unsigned SplitEdges = 0;
  unsigned GuardBranches = 0;
  unsigned LoweredObservations = 0;
  bool CFGChanged = false;
};

/// Backend boundary for generic MAIR materialization. `prepare` must be
/// read-only and reject all recoverable constraints before committed apply.
class MAIRObservationBackend {
public:
  virtual ~MAIRObservationBackend() = default;

  virtual llvm::Error prepare(const MAIRMaterializationPlan &Plan) = 0;
  virtual void emit(const MaterializedObservation &Observation,
                    llvm::IRBuilderBase &Builder) = 0;
};

} // namespace mair
