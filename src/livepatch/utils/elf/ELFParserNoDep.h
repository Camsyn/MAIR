// ElfHParser.h
#pragma once
#include "AbstractELFParser.h"
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

class ElfParser : public AbstractELFParser {
public:
  explicit ElfParser(const std::string &path = DefaultPath)
      : AbstractELFParser(path), fd(-1), data(nullptr), size(0) {}

  ~ElfParser() override {
    if (data)
      munmap(const_cast<void *>(static_cast<const void *>(data)), size);
    if (fd >= 0)
      close(fd);
  }

  void open() override {
    fd = ::open(elf_path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("open failed");
    struct stat st;
    if (fstat(fd, &st) < 0)
      throw std::runtime_error("stat failed");
    size = st.st_size;
    data = reinterpret_cast<const uint8_t *>(
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED)
      throw std::runtime_error("mmap failed");

    // 检查 ELF header
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(data);
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
      throw std::runtime_error("not ELF");

    phdr = reinterpret_cast<const Elf64_Phdr *>(data + ehdr->e_phoff);
    shdr = reinterpret_cast<const Elf64_Shdr *>(data + ehdr->e_shoff);
    shnum = ehdr->e_shnum;
    shstr =
        reinterpret_cast<const char *>(data + shdr[ehdr->e_shstrndx].sh_offset);

    findSymtab();
  }

  uint64_t searchSymbolOffset(const std::string &name) override {
    if (!symtab.len || !strtab.len)
      throw std::runtime_error(".symtab/.strtab missing");
    size_t count = symtab.size() / sizeof(Elf64_Sym);
    auto *syms = reinterpret_cast<const Elf64_Sym *>(symtab.data());
    auto *sym_names = reinterpret_cast<const char *>(strtab.data());
    for (size_t i = 0; i < count; ++i) {
      const Elf64_Word name_offset = syms[i].st_name;
      if (name_offset == 0) {
        continue;
      }
      const char *nm = &sym_names[name_offset];
      if (name == nm && ELF64_ST_TYPE(syms[i].st_info) == STT_FUNC) {
        return syms[i].st_value;
      }
    }
    throw std::runtime_error("symbol not found");
  }

  bool hasSymTab() const override { return symtab.len && strtab.len; }

private:
  struct Section {
    Section() : ptr(nullptr), len(0) {}
    Section(const uint8_t *ptr, size_t len) : ptr(ptr), len(len) {}
    const uint8_t *data() const { return ptr; }
    size_t size() const { return len; }
    const uint8_t *ptr;
    size_t len;
  };

  void findSymtab() {
    for (int i = 0; i < shnum; ++i) {
      const char *name = shstr + shdr[i].sh_name;
      if (shdr[i].sh_type == SHT_SYMTAB) {
        symtab = Section{data + shdr[i].sh_offset, shdr[i].sh_size};
        strtab = Section{data + shdr[shdr[i].sh_link].sh_offset,
                         shdr[shdr[i].sh_link].sh_size};
        break;
      }
    }
  }

  int fd;
  const uint8_t *data;
  size_t size;
  const Elf64_Phdr *phdr;
  const Elf64_Shdr *shdr;
  int shnum;
  const char *shstr;

  Section symtab;
  Section strtab;
};
