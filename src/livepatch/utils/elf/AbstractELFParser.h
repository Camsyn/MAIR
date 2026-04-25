// AbstractELFParser.h
#pragma once

#include <cstdint>
#include <string>


// Now this parser only support the loaded ELF file, i.e., /proc/self/exe
// This implementation does not rely on any external library, such as libelf.
/// TODO: support ELF parsing for shared library. Along with the base address
/// resolution for shared library, we can also support the unexported symbol
/// resolution for shared library.
class AbstractELFParser {
public:
    static constexpr const char* DefaultPath = "/proc/self/exe";

    explicit AbstractELFParser(const std::string& path = DefaultPath)
        : elf_path(path) {}
    virtual ~AbstractELFParser() = default;

    /// 打开 ELF 并初始化，失败时抛出异常或终止
    virtual void open() = 0;
    /// 查找指定符号（函数）的在进程空间中的地址偏移
    virtual uint64_t searchSymbolOffset(const std::string& name) = 0;
    /// 是否存在符号表
    virtual bool hasSymTab() const = 0;

protected:
    std::string elf_path;
};
