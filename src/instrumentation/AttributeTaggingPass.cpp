//===----------------------------------------------------------------------===//
//
// This file is to define a transformation pass that adds attribute tags to
// functions and instructions. Because the sanitizer passes only instrument 
// those functions with appropriate attribute.
// - AddressSanitizer: attribute `sanitize_address`
// - ThreadSanitizer: attribute `sanitize_address`
//
//===----------------------------------------------------------------------===//

#include "AttributeTaggingPass.hpp"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Module.h"

using namespace llvm;


AttributeTaggingPass::AttributeTaggingPass(SanitizerType sanTy): _sanTy(sanTy) {}

PreservedAnalyses AttributeTaggingPass::run(Module &M, ModuleAnalysisManager &_) {
    for (auto &F : M) {
      if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation)) {
          continue;
      }

      const auto &MD = F.getSanitizerMetadata();
      
      switch (_sanTy) {
      case ASan:
          // check whether the function has attribute 'no_sanitize_address'
          if (F.hasSanitizerMetadata() && MD.NoAddress) {
              break;
          }
          F.addFnAttr(Attribute::SanitizeAddress);   
        break;
      case TSan:
          F.addFnAttr(Attribute::SanitizeThread);
        break;
      case MSan:
          F.addFnAttr(Attribute::SanitizeMemory);
        break;
      case XSan:
          // FIXME: do not hard code embedding!
          F.addFnAttr(Attribute::SanitizeAddress);   
          F.addFnAttr(Attribute::SanitizeThread);
          F.addFnAttr(Attribute::SanitizeMemory);
        break;
      case UBSan:
      case SanNone:
      default:
        break;
      }
    }
    
    return PreservedAnalyses::all();
}
