#include "MetaDataUtils.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Metadata.h>
#include <mutex>

using namespace llvm;

namespace __xsan {

// ---------------------- MetaDataHelper -------------------------

template <typename MetaT, typename InstT>
void MetaDataHelper<MetaT, InstT, false>::set(InstT &I) {
  // We cannot use func-local static var to init the Ctx, as it might differ for
  // different Modules.
  llvm::LLVMContext &Ctx = I.getContext();
  if (!ID)
    ID = Ctx.getMDKindID(MetaT::Name);
  if (is(I))
    return;
  I.setMetadata(ID, llvm::MDNode::get(Ctx, {}));
}

template <typename MetaT, typename InstT>
auto MetaDataHelper<MetaT, InstT, true>::get(const InstT &I)
    -> std::optional<Extra> {
  MDNode *MD;
  if (!ID || !(MD = I.getMetadata(ID)))
    return std::nullopt;
  if constexpr (UseDataMap<MetaT>::value)
    return MetaT::DataMap.lookup(&I);
  else
    return MetaDataExtra<Extra>::unpack(MD);
}

template <typename MetaT, typename InstT>
void MetaDataHelper<MetaT, InstT, true>::set(InstT &I, const Extra &Data) {
  // We cannot use func-local static var to init the Ctx, as it might differ for
  // different Modules.
  llvm::LLVMContext &Ctx = I.getContext();
  if (!ID)
    ID = Ctx.getMDKindID(MetaT::Name);
  MDNode *MD;
  if constexpr (UseDataMap<MetaT>::value) {
    /// Thread-safe
    static std::mutex Mutex;
    // If the type has an associated member 'DataMap', we use it to store the
    // data.
    {
      // Only lock the write, because read must be after write.
      std::lock_guard<std::mutex> Lock(Mutex);
      MetaT::DataMap[&I] = Data;
    }
    MD = llvm::MDNode::get(Ctx, {});
  } else {
    // Otherwise, we use MDNode to store the data.
    MD = MetaDataExtra<Extra>::pack(Ctx, Data);
  }
  I.setMetadata(ID, MD);
}

template <typename MetaT, typename InstT>
void MetaDataHelper<MetaT, InstT, true>::clear() {
  if constexpr (UseDataMap<MetaT>::value)
    MetaT::DataMap.clear();
}

template <typename MetaT, typename InstT>
llvm::MDNode *MetaDataHelper<MetaT, InstT, true>::getMD(const InstT &I) {
  return ID ? I.getMetadata(ID) : nullptr;
}

template <typename MetaT, typename InstT>
void MetaDataHelper<MetaT, InstT, true>::setMD(InstT &I, MDNode *MD) {
  if (ID)
    I.setMetadata(ID, MD);
}

// ---------------------- Operand Bundle Helper -------------------------

template <typename MetaT, typename InstT, typename Enabler>
llvm::OperandBundleDef
OperandBundleHelper<MetaT, InstT, Enabler>::getBundle(const Extra &Data) {
  return OperandBundleDef(MetaT::Name, pack(Data.getContext(), Data));
}

template <typename MetaT, typename CallT, typename Enabler>
std::optional<typename MetaT::Extra>
OperandBundleHelper<MetaT, CallT, Enabler>::get(const CallT &I) {
  if (auto *Bundle = I.getOperandBundle(MetaT::Name))
    return unpack(*Bundle);
  return std::nullopt;
}

template <typename MetaT, typename CallT, typename Enabler>
llvm::CallBase *
OperandBundleHelper<MetaT, CallT, Enabler>::replaceCallWithExtra(
    CallT &I, const Extra &Data) {
  OperandBundleDef OB = getBundle(Data);
  CallBase *NewCall = CallBase::addOperandBundle(&I, -1, OB, &I);
  NewCall->copyMetadata(I);
  I.replaceAllUsesWith(NewCall);
  I.eraseFromParent();
  return NewCall;
}

// ---------------------- Replaced Alloca -------------------------

template <>
MDNode *
MetaDataExtra<ReplacedAllocaExtra>::pack(LLVMContext &Ctx,
                                         const ReplacedAllocaExtra &Data) {
  ConstantInt *LenConst = ConstantInt::get(Type::getInt64Ty(Ctx), Data.Len);
  ConstantInt *AlignConst =
      ConstantInt::get(Type::getInt64Ty(Ctx), Data.Align.value());

  Metadata *LenMetadata = ConstantAsMetadata::get(LenConst);
  Metadata *AlignMetadata = ConstantAsMetadata::get(AlignConst);
  // Metadata *ArgMetadata = Data.Arg ? ValueAsMetadata::get(Data.Arg) :
  // nullptr;
  return MDNode::get(Ctx, {LenMetadata, AlignMetadata});
}

template <>
ReplacedAllocaExtra MetaDataExtra<ReplacedAllocaExtra>::unpack(MDNode *MD) {
  ReplacedAllocaExtra Data;

  Metadata *LenMD = MD->getOperand(0).get();
  ConstantAsMetadata *LenConstMD = cast<ConstantAsMetadata>(LenMD);
  ConstantInt *LenConstInt = cast<ConstantInt>(LenConstMD->getValue());
  Data.Len = LenConstInt->getZExtValue();

  Metadata *AlignMD = MD->getOperand(1).get();
  ConstantAsMetadata *AlignConstMD = cast<ConstantAsMetadata>(AlignMD);
  ConstantInt *AlignConstInt = cast<ConstantInt>(AlignConstMD->getValue());
  Data.Align = AlignConstInt->getAlignValue();

  // Metadata *ArgMD = MD->getOperand(2).get();
  // if (auto *ValMD = ArgMD ? dyn_cast<ValueAsMetadata>(ArgMD) : nullptr)
  //   Data.Arg = cast<Argument>(ValMD->getValue());
  // else
  //   Data.Arg = nullptr;

  return Data;
}

// ---------------------- Replaced Atomic -------------------------

llvm::DenseMap<llvm::Instruction *, ReplacedAtomicExtra>
    ReplacedAtomicMeta::DataMap;

// --------------- MetaDataHelper Specializations --------------------

/// Explicit Specialization
// template class MetaDataHelper<UBSanInstMeta>;
/// Explicit Declaration
// template<> class MetaDataHelper<UBSanInstMeta>;
#define INSTANTIATE_META_DATA_HELPER(...)                                      \
  template class MetaDataHelperBase<__VA_ARGS__>;                              \
  template class MetaDataHelper<__VA_ARGS__>;

#define INSTANTIATE_OPERAND_BUNDLE_HELPER(...)                                 \
  template class OperandBundleHelper<__VA_ARGS__>;

INSTANTIATE_META_DATA_HELPER(DelegateToXSanMeta)
INSTANTIATE_META_DATA_HELPER(ReplacedAllocaMeta)
INSTANTIATE_META_DATA_HELPER(CopyArgsMeta, llvm::MemCpyInst)
INSTANTIATE_META_DATA_HELPER(ReplacedAtomicMeta)
INSTANTIATE_META_DATA_HELPER(UBSanInstMeta)

#undef INSTANTIATE_META_DATA_HELPER
#undef INSTANTIATE_OPERAND_BUNDLE_HELPER

} // namespace __xsan
