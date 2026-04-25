#include <string>

// Return true if the executable file is stripped, i.e.,
// symtab is not available.
bool isSelfProcStripped();

// If the exec is not stripped, then we can find the symbol bias in the
// symtab.
// Along with resolution of the base address, we can thus get the symbol
// address.
void *find_symtab_symbol_addr(const std::string &symbol_name);
