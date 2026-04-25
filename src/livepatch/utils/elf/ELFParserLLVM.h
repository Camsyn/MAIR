// LLVMELFParser.h
#pragma once
#include "AbstractELFParser.h"
#include <llvm/Object/ELF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <memory>
#include <system_error>

using namespace llvm;
using namespace llvm::object;

/// Rely on LLVMObject/LLVM
class LLVMELFParser : public AbstractELFParser {
  static OwningBinary<Binary> loadBinary(const std::string &path) {
    auto bin = createBinary(path);
    if (!bin)
      throw std::runtime_error("LLVM createBinary failed");
    return std::move(*bin);
  }

public:
  explicit LLVMELFParser(const std::string &path = DefaultPath)
      : AbstractELFParser(path), bin(std::move(loadBinary(path))) {
    open();
  }

  void open() override {
    obj = dyn_cast<ELF64LEObjectFile>(bin.getBinary());
    if (!obj)
      throw std::runtime_error("not ELF64LE");
  }

  uint64_t searchSymbolOffset(const std::string &name) override {
    for (const SymbolRef &sym : obj->symbols()) {
      Expected<StringRef> sname = sym.getName();
      if (!sname)
        continue;
      if (sname->equals(name)) {
        Expected<uint64_t> addr = sym.getAddress();
        if (!addr)
          continue;
        return *addr;
      }
    }
    throw std::runtime_error("symbol not found");
  }

  bool hasSymTab() const override {
    return any_of(obj->sections(), [](const SectionRef &sec) {
      Expected<StringRef> nameOrErr = sec.getName();
      if (!nameOrErr)
        return false;
      return nameOrErr->equals(".symtab");
    });
  }

private:
  OwningBinary<Binary> bin;
  const ELF64LEObjectFile *obj;
};
