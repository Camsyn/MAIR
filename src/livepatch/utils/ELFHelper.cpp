#include "PatchCore.h"
#include "config_compile.h"

using namespace __xsan;

// These macros are defined in the CMakeLists.txt file
#if defined(HAS_LLVM_OBJECT)
#include "elf/ELFParserLLVM.h"
// Rely on LLVM
// Should we use `ElfParser`, which rely on nothing but a header file.
static LLVMELFParser Parser;
#elif defined(HAS_LIBELF)
#include "elf/ELFParserLibElf.h"
static LibElfParser Parser;
#else
#include "elf/ELFParserNoDep.h"
static ElfParser Parser;
#endif

bool isSelfProcStripped() { return !Parser.hasSymTab(); }

// Return the address of the symbol in current process via ELF resolution
void *find_symtab_symbol_addr(const std::string &symbol_name) {
  uint64_t symbol_offset = Parser.searchSymbolOffset(symbol_name);
  void *base_address = get_base_address();
  void *symbol_address = (char *)base_address + symbol_offset;
  return symbol_address;
}
