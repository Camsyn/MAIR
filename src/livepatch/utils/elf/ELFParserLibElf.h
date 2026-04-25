// LibElfParser.h
#pragma once
#include "AbstractELFParser.h"
#include <cstring>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdexcept>
#include <unistd.h>

// Rely on libelf
class LibElfParser : public AbstractELFParser {
public:
  explicit LibElfParser(const std::string &path = DefaultPath)
      : AbstractELFParser(path), fd(-1), e(nullptr), symtab(nullptr) {
    open();
  }

  ~LibElfParser() override {
    if (e)
      elf_end(e);
    if (fd >= 0)
      close(fd);
  }

  void open() override {
    if ((fd = ::open(elf_path.c_str(), O_RDONLY)) < 0)
      throw std::runtime_error(std::string("open failed: ") +
                               std::strerror(errno));
    if (elf_version(EV_CURRENT) == EV_NONE)
      throw std::runtime_error("libelf init failed");
    e = elf_begin(fd, ELF_C_READ, nullptr);
    if (!e)
      throw std::runtime_error("elf_begin failed");
    if (elf_kind(e) != ELF_K_ELF)
      throw std::runtime_error("not an ELF object");
    if (!gelf_getehdr(e, &ehdr))
      throw std::runtime_error("getehdr failed");
    symtab = parseSymtab();
  }

  uint64_t searchSymbolOffset(const std::string &name) override {
    if (!symtab)
      symtab = parseSymtab();
    if (!symtab)
      throw std::runtime_error("no .symtab");
    size_t n = shdr.sh_size / shdr.sh_entsize;
    for (size_t i = 0; i < n; ++i) {
      GElf_Sym sym;
      if (gelf_getsym(symtab, i, &sym) != &sym)
        continue;
      const char *nm = elf_strptr(e, shdr.sh_link, sym.st_name);
      if (!nm || name != nm)
        continue;
      if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
        continue;
      return sym.st_value;
    }
    throw std::runtime_error("symbol not found");
  }

  bool hasSymTab() const override { return symtab != nullptr; }

private:
  Elf_Data *parseSymtab() {
    Elf_Scn *scn = nullptr;
    while ((scn = elf_nextscn(e, scn))) {
      if (gelf_getshdr(scn, &shdr) && shdr.sh_type == SHT_SYMTAB) {
        return elf_getdata(scn, nullptr);
      }
    }
    return nullptr;
  }

  int fd;
  Elf *e;
  GElf_Ehdr ehdr;
  GElf_Shdr shdr;
  Elf_Data *symtab;
};
