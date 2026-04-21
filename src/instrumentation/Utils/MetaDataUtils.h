#pragma once

#include "xsan_llvm_compat.h"
#include <llvm/IR/Argument.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Value.h>
#include <optional>

namespace __xsan {

// ---------------------- Helper Definitions -------------------------

// If the type has an associated member 'Name', return true; Otherwise, return
// false.
template <typename MetaT, typename = void>
struct IsMetaData : std::false_type {};
template <typename MetaT>
struct IsMetaData<MetaT, std::void_t<decltype(MetaT::Name)>> : std::true_type {
};

// If the type has an associated member 'DataMap', return true; Otherwise,
// return false.
template <typename MetaT, typename = void>
struct UseDataMap : std::false_type {};
template <typename MetaT>
struct UseDataMap<MetaT, std::void_t<decltype(MetaT::DataMap)>>
    : std::true_type {};

// ---------------------- MetaData ----------------------------

template <typename ExtraT> struct MetaDataExtra {
  using Extra = ExtraT;
  static llvm::MDNode *pack(llvm::LLVMContext &Ctx, const ExtraT &Data);
  static ExtraT unpack(llvm::MDNode *MD);
};

// If the type has an associated type 'Extra' and is a subclass of
// MetaDataExtra<Extra>, return true; Otherwise, return false.
template <typename MetaT, typename = void>
struct HasMetaDataExtra : std::false_type {};
template <typename MetaT>
struct HasMetaDataExtra<MetaT, std::void_t<typename MetaT::Extra>>
    : std::bool_constant<
          std::is_base_of_v<MetaDataExtra<typename MetaT::Extra>, MetaT>> {};

/// MetaT is only used to distinguish between different static members and does
/// not participate in the actual implementation
template <typename MetaT, typename InstT = llvm::Instruction>
class MetaDataHelperBase {
public:
  static bool is(const InstT &I) { return ID && I.hasMetadata(ID); }
  static void drop(InstT &I) {
    if (ID)
      I.setMetadata(ID, nullptr);
  }

protected:
  static unsigned ID;
};

template <typename MetaT, typename InstT>
unsigned MetaDataHelperBase<MetaT, InstT>::ID = 0;

template <typename MetaT, typename InstT = llvm::Instruction,
          bool HasExtra = HasMetaDataExtra<MetaT>::value,
          typename = std::enable_if_t<IsMetaData<MetaT>::value>>
class MetaDataHelper;

template <typename MetaT, typename InstT>
class MetaDataHelper<MetaT, InstT, false>
    : public MetaDataHelperBase<MetaT, InstT> {
  using MetaDataHelperBase<MetaT, InstT>::ID;

public:
  using MetaDataHelperBase<MetaT, InstT>::is;
  using MetaDataHelperBase<MetaT, InstT>::drop;

  static void set(InstT &I);
};

template <typename MetaT, typename InstT>
class MetaDataHelper<MetaT, InstT, true>
    : public MetaDataHelperBase<MetaT, InstT> {
  using MetaDataHelperBase<MetaT, InstT>::ID;

public:
  using MetaDataHelperBase<MetaT, InstT>::is;
  using MetaDataHelperBase<MetaT, InstT>::drop;
  using Extra = typename MetaT::Extra;

  static std::optional<Extra> get(const InstT &I);
  static void set(InstT &I, const Extra &Data);
  /// Use MDNode to store and recover the semantic
  /// Specially, for CallBase, it is a more natural way to
  /// use operand bundles, as follows:
  ///    OperandBundleDef Bundle("my.atomic.bundle", {Values, ...});
  ///    IRB.CreateCall(Callee, Args, {Bundle});
  /// However, for the sake of unity, we ultimately adopted an anonymous MDNode
  /// to implement this feature
  static llvm::MDNode *getMD(const InstT &I);
  static void setMD(InstT &I, llvm::MDNode *MD);
  // Clear the metadata of instructions, do nothing by default
  static void clear();
};

// --------------------- Operand Bundle ----------------------------
/// TODO: implement this if required
template <typename ExtraT> struct OperandBundleExtra {
  using Extra = ExtraT;
  static llvm::SmallVector<llvm::Value *, 4> pack(llvm::LLVMContext &Ctx,
                                                  const ExtraT &Data);
  static ExtraT unpack(const llvm::OperandBundleUse &CB);
};

// If the type has an associated type 'Extra' and is a subclass of
// MetaDataExtra<Extra>, return true; Otherwise, return false.
template <typename MetaT, typename = void>
struct HasOperandBundleExtra : std::false_type {};
template <typename MetaT>
struct HasOperandBundleExtra<MetaT, std::void_t<typename MetaT::Extra>>
    : std::bool_constant<
          std::is_base_of_v<OperandBundleExtra<typename MetaT::Extra>, MetaT>> {
};

/// This helper store the extra data in the operand bundle of the call
/// instruction. For CallBase, it is a more natural way to use operand bundles,
/// as follows:
///    OperandBundleDef Bundle("my.atomic.bundle", {Values, ...});
///    IRB.CreateCall(Callee, Args, {Bundle});
template <typename MetaT, typename CallT,
          typename =
              std::enable_if_t<IsMetaData<MetaT>::value &&
                               std::is_base_of_v<llvm::CallBase, CallT> &&
                               HasOperandBundleExtra<MetaT>::value>>
class OperandBundleHelper {
public:
  using Extra = typename MetaT::Extra;
  static bool is(const CallT &I) {
    return I.getOperandBundle(MetaT::Name) != nullptr;
  }
  static llvm::OperandBundleDef getBundle(const Extra &Data);
  static std::optional<Extra> get(const CallT &I);
  // We cannot add a new operand bundle to CallBase, so we need to replace the
  // call instruction with a new one.
  static llvm::CallBase *replaceCallWithExtra(CallT &I, const Extra &Data);
};

// ---------------------- Delegated to XSan -------------------------

struct DelegateToXSanMeta {
  static constexpr char Name[] = "xsan.delegate";
};

// ---------------------- Replaced Alloca -------------------------

struct ReplacedAllocaExtra {
  size_t Len;
  llvm::Align Align;
  /// `Argument *Arg` is invalid, if required, use param idx instead.
  /// Argument *Arg;
};

struct ReplacedAllocaMeta : MetaDataExtra<ReplacedAllocaExtra> {
  static constexpr char Name[] = "replaced.alloca";
};

// ---------------------- Copy Args -------------------------

struct CopyArgsMeta {
  static constexpr char Name[] = "copy.args";
};

// ---------------------- Replaced Atomic -------------------------

struct ReplacedAtomicExtra {
  enum AtomicType { Load, Store, RMW, CAS };

  AtomicType Type;
  const llvm::Use *AddrUse;
  const llvm::Use *ValUse;
  llvm::MaybeAlign Align; // valid for load/store
};

struct ReplacedAtomicMeta : MetaDataExtra<ReplacedAtomicExtra> {
  static constexpr char Name[] = "replaced.atomic";
  // Need to store function-local values, which could not be stored in MDNode
  // Therefore, we use Map to them.
  static llvm::DenseMap<llvm::Instruction *, ReplacedAtomicExtra> DataMap;
};

// ---------------------- User IR --------------------------------

struct UserIRMeta {
  static constexpr char Name[] = "xsan.user";
};

// ---------------------- UBSan Instrumented -------------------------

struct UBSanInstMeta {
  static constexpr char Name[] = "xsan.ubsan";
};

// ---------------------- ASan Instrumented -------------------------

struct ASanInstMeta {
  static constexpr char Name[] = "xsan.asan";
};

// ---------------------- TSan Instrumented -------------------------

struct TSanInstMeta {
  static constexpr char Name[] = "xsan.tsan";
};

// ---------------------- MSan Instrumented -------------------------

struct MSanInstMeta {
  static constexpr char Name[] = "xsan.msan";
};

// ---------------------- NoSanitize --------------------------------

struct NoSanitizeMeta {
  static constexpr char Name[] = "nosanitize";
};

// --------------- MetaDataHelper Specializations --------------------

/// Specializations for nosanitize
template <> class MetaDataHelper<NoSanitizeMeta> {
  static constexpr unsigned ID = llvm::LLVMContext::MD_nosanitize;

public:
  static bool is(const llvm::Instruction &I) { return I.hasMetadata(ID); }
  static void drop(llvm::Instruction &I) { I.setMetadata(ID, nullptr); }
  static void set(llvm::Instruction &I) {
    I.setMetadata(ID, llvm::MDNode::get(I.getContext(), {}));
  }
};

using DelegateToXSan = MetaDataHelper<DelegateToXSanMeta>;
using ReplacedAlloca = MetaDataHelper<ReplacedAllocaMeta>;
using CopyArgs = MetaDataHelper<CopyArgsMeta, llvm::MemCpyInst>;
using ReplacedAtomic = MetaDataHelper<ReplacedAtomicMeta>;
using UserIR = MetaDataHelper<UserIRMeta>;
using UBSanInst = MetaDataHelper<UBSanInstMeta>;
using ASanInst = MetaDataHelper<ASanInstMeta>;
using TSanInst = MetaDataHelper<TSanInstMeta>;
using MSanInst = MetaDataHelper<MSanInstMeta>;
using NoSanitize = MetaDataHelper<NoSanitizeMeta>;

/// ---------------------- Util Functions ----------------------------

static inline bool isReplacedInst(const llvm::Instruction &I) {
  return ReplacedAlloca::is(I) || ReplacedAtomic::is(I);
}

} // namespace __xsan
