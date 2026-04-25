/*
Patch the link_command_spec in gcc.c / gcc.cc in gcc.
*/

/*
The complete linker spec in gcc-9.4 is as follows:

"%{!fsyntax-only:%{!c:%{!M:%{!MM:%{!E:%{!S:    %(linker)
%{!fno-use-linker-plugin:%{!fno-lto:     -plugin %(linker_plugin_file)
-plugin-opt=%(lto_wrapper)
-plugin-opt=-fresolution=%u.res
%{flinker-output=*:-plugin-opt=-linker-output-known}
%{!nostdlib:%{!nodefaultlibs:%:pass-through-libs(%(link_gcc_c_sequence))}}
}}%{flto|flto=*:%<fcompare-debug*}     %{flto} %{fno-lto} %{flto=*} %l
%{static|shared|r:;pie:-pie} %{fuse-ld=*:-fuse-ld=%*}
%{gz|gz=zlib:--compress-debug-sections=zlib}
%{gz=none:--compress-debug-sections=none}
%{gz=zlib-gnu:--compress-debug-sections=zlib-gnu} %X %{o*} %{e*} %{N} %{n} %{r}
%{s} %{t} %{u*} %{z} %{Z} %{!nostdlib:%{!r:%{!nostartfiles:%S}}}
%{static|no-pie|static-pie:} %@{L*} %(mfwrap) %(link_libgcc)
%{fvtable-verify=none:} %{fvtable-verify=std:   %e-fvtable-verify=std is not
supported in this configuration} %{fvtable-verify=preinit:
%e-fvtable-verify=preinit is not supported in this configuration}
%{!nostdlib:%{!r:%{!nodefaultlibs:%{%:sanitize(address):%{!shared:libasan_preinit%O%s}
%{static-libasan:%{!shared:-Bstatic --whole-archive -lasan --no-whole-archive
-Bdynamic}}%{!static-libasan:-lasan}}
%{%:sanitize(hwaddress):%{static-libhwasan:%{!shared:-Bstatic --whole-archive
-lhwasan --no-whole-archive -Bdynamic}}%{!static-libhwasan:-lhwasan}}
%{%:sanitize(thread):%{!shared:libtsan_preinit%O%s}
%{static-libtsan:%{!shared:-Bstatic --whole-archive -ltsan --no-whole-archive
-Bdynamic}}%{!static-libtsan:-ltsan}}
%{%:sanitize(leak):%{!shared:liblsan_preinit%O%s}
%{static-liblsan:%{!shared:-Bstatic --whole-archive -llsan --no-whole-archive
-Bdynamic}}%{!static-liblsan:-llsan}}}}}
%o
%{fopenacc|fopenmp|%:gt(%{ftree-parallelize-loops=*:%*}
1):\t%:include(libgomp.spec)%(link_gomp)}
%{fgnu-tm:%:include(libitm.spec)%(link_itm)}    %(mflib)
%{fsplit-stack:--wrap=pthread_create}
%{fprofile-arcs|fprofile-generate*|coverage:-lgcov}
%{!nostdlib:%{!r:%{!nodefaultlibs:%{%:sanitize(address):
%{static-libasan|static:%:include(libsanitizer.spec)%(link_libasan)}
%{static:%ecannot specify -static with -fsanitize=address}}
%{%:sanitize(hwaddress):
%{static-libhwasan|static:%:include(libsanitizer.spec)%(link_libhwasan)}\t
%{static:%ecannot specify -static with -fsanitize=hwaddress}}
%{%:sanitize(thread):
%{static-libtsan|static:%:include(libsanitizer.spec)%(link_libtsan)}
%{static:%ecannot specify -static with -fsanitize=thread}}
%{%:sanitize(undefined):%{static-libubsan:-Bstatic} -lubsan
%{static-libubsan:-Bdynamic}
%{static-libubsan|static:%:include(libsanitizer.spec)%(link_libubsan)}}
%{%:sanitize(leak):
%{static-liblsan|static:%:include(libsanitizer.spec)%(link_liblsan)}}}}}
%{!nostdlib:%{!r:%{!nodefaultlibs:%(link_ssp) %(link_gcc_c_sequence)}}}
%{!nostdlib:%{!r:%{!nostartfiles:%E}}} %{T*}  \n%(post_link) }}}}}}"


The key components related to sanitizers are:
// handle sanitizer linking
%{!nostdlib:%{!r:%{!nodefaultlibs:
    %{%:sanitize(address):
        %{!shared:libasan_preinit%O%s}
        %{static-libasan:
            %{!shared:-Bstatic --whole-archive -lasan --no-whole-archive
-Bdynamic}
        }
        %{!static-libasan:-lasan}
    }
    %{%:sanitize(hwaddress):
        %{static-libhwasan:
            %{!shared:-Bstatic --whole-archive -lhwasan --no-whole-archive
-Bdynamic}
        }
        %{!static-libhwasan:-lhwasan}
    }
    %{%:sanitize(thread):
        %{!shared:libtsan_preinit%O%s}
        %{static-libtsan:
            %{!shared:-Bstatic --whole-archive -ltsan --no-whole-archive
-Bdynamic}
        }
        %{!static-libtsan:-ltsan}
    }
    %{%:sanitize(leak):
        %{!shared:liblsan_preinit%O%s}
        %{static-liblsan:
            %{!shared:-Bstatic --whole-archive -llsan --no-whole-archive
-Bdynamic}
        }
        %{!static-liblsan:-llsan}
    }
}}}
%o // target output file
// handle sanitizer dependency
%{!nostdlib:%{!r:%{!nodefaultlibs:
    %{%:sanitize(address):
        %{static-libasan|static:%:include(libsanitizer.spec)%(link_libasan)}
        %{static:%ecannot specify -static with -fsanitize=address}
    }
    %{%:sanitize(hwaddress):
        %{static-libhwasan|static:%:include(libsanitizer.spec)%(link_libhwasan)}\t
        %{static:%ecannot specify -static with -fsanitize=hwaddress}}
    %{%:sanitize(thread):
        %{static-libtsan|static:%:include(libsanitizer.spec)%(link_libtsan)}
        %{static:%ecannot specify -static with -fsanitize=thread}
    }
    %{%:sanitize(undefined):
        %{static-libubsan:-Bstatic}
        -lubsan
        %{static-libubsan:-Bdynamic}
        %{static-libubsan|static:%:include(libsanitizer.spec)%(link_libubsan)}
    }
    %{%:sanitize(leak):
        %{static-liblsan|static:%:include(libsanitizer.spec)%(link_liblsan)}
    }
}}}

*/

#include "PatchCore.h"
#include "config_compile.h"
#include "xsan_common.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <type_traits>
#include <vector>

using namespace __xsan;

namespace {
void *search_substr(const char *hay, size_t haylen, const char *needle,
                    size_t neelen) {
  if (neelen == 0 || haylen < neelen)
    return nullptr;
  auto it = std::search(hay, hay + haylen, needle, needle + neelen);
  if (it == hay + haylen)
    return nullptr;
  return (void *)it;
}

std::string_view find_link_command_spec(const std::vector<ROSegment> &ro_sec) {
  static constexpr std::array patterns{
      "%(linker)",
      "%{flto} %{fno-lto}",
      "%(post_link)",
  };
  for (auto [ro, rosz] : ro_sec) {
    char *p = (char *)search_substr(ro, rosz, patterns[0], strlen(patterns[0]));
    if (!p)
      continue;
    std::string_view spec(p);
    std::string_view subspec = spec;

    for (int i = 1; i < patterns.size(); ++i) {
      auto pos = subspec.find(patterns[i]);
      if (pos == std::string_view::npos) {
        subspec = std::string_view();
        break;
      }
      subspec = subspec.substr(pos + strlen(patterns[i]));
    }
    if (subspec.empty()) {
      continue;
    }
    return spec;
  }
  return std::string_view();
}

class NewLinkSpecBuilder {
  using FrToPair = std::pair<std::string_view, std::string_view>;
  friend class SpecComp;

public:
  NewLinkSpecBuilder(std::string_view spec) : link_spec(spec) {}
  std::string build() {
    if (delta_size > 0) {
      /// TODO: figure out is delta_size > 0 is safe to use. Otherwise, we
      /// cannot delegate the runtime linking to the livepatch completely
      /// (refer to clang's livepath -- CommonArgs.cpp), still requires
      /// compiler wrapper to do the runtime linking. Because the whole
      /// new spec to link XSan is too long, referring to comments of
      /// XsanRtSpecCompBuilder for details.
      FATAL("Dangerous operation: rewrite ro string with larger size which "
            "might overwrite other strings.");
    }
    // Sort operations by the starting position of from in link_spec,
    // to ensure the correct replacement order
    std::sort(operations.begin(), operations.end(),
              [this](const FrToPair &a, const FrToPair &b) {
                return (a.first.data() - link_spec.data()) <
                       (b.first.data() - link_spec.data());
              });

    std::string result;
    result.reserve(link_spec.size() + delta_size);
    size_t last = 0;
    const char *start = link_spec.data();
    for (const auto &[from, to] : operations) {
      size_t pos = from.data() - start;
      // Skip invalid operations
      if (pos < last || pos > link_spec.size())
        continue;
      // Copy the previous fragment
      result.append(link_spec.substr(last, pos - last));
      // Replace with to
      result.append(to);
      last = pos + from.size();
    }
    // Copy the remaining part
    if (last < link_spec.size()) {
      result.append(link_spec.substr(last));
    }
    operations.clear();
    delta_size = 0;
    return result;
  }

private:
  void insert(size_t pos, std::string_view str) {
    if (str.empty())
      return;
    std::string_view from = link_spec.substr(pos, 0);
    replace(from, str);
  }

  void insert(const char *p, std::string_view str) {
    size_t pos = p - link_spec.data();
    insert(pos, str);
  }

  void remove(std::string_view str) {
    if (str.empty())
      return;
    replace(str, std::string_view());
  }

  void replace(std::string_view from, std::string_view to) {
    if (!inSpec(from))
      return;
    delta_size += (ssize_t)to.size() - (ssize_t)from.size();
    operations.emplace_back(from, to);
  }

  bool inSpec(std::string_view str) const {
    if (str.data() < link_spec.data() ||
        str.data() >= link_spec.data() + link_spec.size()) {
      return false;
    }
    size_t sz = str.size();
    if (str.data() + sz > link_spec.data() + link_spec.size()) {
      return false;
    }
    return true;
  }

  std::vector<FrToPair> operations;
  ssize_t delta_size = 0;
  std::string_view link_spec;
};

class SpecComp {
public:
  bool isEmpty() const { return comp.empty(); }
  void remove(NewLinkSpecBuilder &builder) const { builder.remove(comp); }
  void replace(NewLinkSpecBuilder &builder, std::string_view to) const {
    builder.replace(comp, to);
  }
  void appendAfter(NewLinkSpecBuilder &builder, std::string_view to) const {
    builder.insert(comp.data() + comp.size(), to);
  }
  void appendIn(NewLinkSpecBuilder &builder, std::string_view to) const {
    builder.insert(comp.data() + comp.size() - depth, to);
  }
  std::string_view getComp() const { return comp; }

protected:
  static std::string_view extract(std::string_view spec,
                                  std::string_view header) {
    size_t pos = spec.find(header);
    if (pos == std::string_view::npos) {
      return std::string_view();
    }
    size_t depth = std::count(header.begin(), header.end(), '{');

    spec = spec.substr(pos);
    const char *it = spec.data() + header.size();
    while (*it && depth > 0) {
      if (*it == '%' && *(it + 1) == '{') {
        depth++;
        ++it;
      } else if (*it == '}') {
        depth--;
      }
      ++it;
    }
    if (depth == 0) {
      return spec.substr(0, it - spec.data());
    }
    return std::string_view();
  }

  SpecComp(std::string_view spec) : SpecComp(spec, std::string_view()) {}
  SpecComp(std::string_view spec, std::string_view hdr)
      : comp(spec), hdr(hdr), depth(std::count(hdr.begin(), hdr.end(), '{')) {}

  std::string_view comp;
  // The header of this spec comp, i.e., %{%:sanitize(address):
  std::string_view hdr;
  // depth of header, i.e., # of unclosed '{' in header
  size_t depth;
};

// link spec of "%{%:sanitize(address): ... }"
class SanitizerComp : public SpecComp {
  friend class SanitzerSpec;

public:
  enum class Type {
    ASan,
    LSan,
    TSan,
    HWASan,
    UBSan,
    Unknown,
  };

  // E.g., "%{%:sanitize(address): ... }"
  static constexpr const char *Header = "%{%:sanitize(";

  bool isType(SanitizerType sanTy) const {
    switch (sanTy) {
    case ASan:
      return type == Type::ASan;
    case TSan:
      return type == Type::TSan;
    case UBSan:
      return type == Type::UBSan;
    default:
      return false;
    }
  }

private:
  static constexpr std::pair<const char *, Type> SanitizerTypes[] = {
      {"address", Type::ASan},    {"thread", Type::TSan},
      {"leak", Type::LSan},       {"hwaddress", Type::HWASan},
      {"undefined", Type::UBSan},
  };

  static std::vector<SanitizerComp> extract(std::string_view san_spec) {
    std::vector<SanitizerComp> ret;
    std::string_view hdr = Header;
    std::string_view comp = SpecComp::extract(san_spec, hdr);
    while (!comp.empty()) {
      for (auto [name, type] : SanitizerTypes) {
        // E.g., %{%:sanitize(address): ... } --> address): ... }
        std::string_view sub = comp.substr(hdr.size());
        // E.g., address): ... } --> address
        sub = sub.substr(0, sub.find_first_of(')'));
        if (sub == name) {
          ret.push_back({comp, type});
          break;
        }
      }
      size_t next_pos = comp.data() - san_spec.data() + comp.size();
      san_spec = san_spec.substr(next_pos);
      comp = SpecComp::extract(san_spec, Header);
    }
    return ret;
  }

  SanitizerComp(std::string_view spec, Type type)
      : SpecComp(spec, Header), type(type) {}

  Type type;
};

// link spec of "%{!nostdlib:%{!r:%{!nodefaultlibs: .... }}}"
class SanitzerSpec : public SpecComp {
  friend class LinkSpec;

public:
  enum class Type {
    // Specify the sanitizer library to link
    RtSpec,
    // Specify the dep to link while linking the sanitizer statically
    DepSpec,
    Other,
  };

  static constexpr const char *Header = "%{!nostdlib:%{!r:%{!nodefaultlibs:";
  static constexpr const char *RtSpecFeature = "-lasan";
  static constexpr const char *DepSpecFeature = "%{link_libasan}";

  bool isRtSpec() const { return type == Type::RtSpec; }
  bool isDepSpec() const { return type == Type::DepSpec; }
  bool isOther() const { return type == Type::Other; }

  std::optional<SanitizerComp> getSanitizerComp(SanitizerType sanTy) const {
    auto it = std::find_if(
        sanitizer_comps.begin(), sanitizer_comps.end(),
        [sanTy](const SanitizerComp &comp) { return comp.isType(sanTy); });
    if (it != sanitizer_comps.end()) {
      return *it;
    }
    return std::nullopt;
  }

private:
  static std::vector<SanitzerSpec> extract(std::string_view spec) {
    std::vector<SanitzerSpec> ret;
    std::string_view comp = SpecComp::extract(spec, Header);
    while (!comp.empty()) {
      if (comp.find(RtSpecFeature) != std::string_view::npos) {
        ret.push_back({comp, Type::RtSpec});
      } else if (comp.find(DepSpecFeature) != std::string_view::npos) {
        ret.push_back({comp, Type::DepSpec});
      } else {
        ret.push_back({comp, Type::Other});
      }
      size_t next_pos = comp.data() - spec.data() + comp.size();
      spec = spec.substr(next_pos);
      comp = SpecComp::extract(spec, Header);
    }
    return ret;
  }

  SanitzerSpec(std::string_view comp, Type type)
      : SpecComp(comp, Header), type(type) {
    sanitizer_comps = SanitizerComp::extract(comp);
  }
  SanitzerSpec() : SpecComp(std::string_view()), type(Type::Other) {}

  std::vector<SanitizerComp> sanitizer_comps;
  Type type;
};

class LinkSpec : SpecComp {
public:
  LinkSpec(std::string_view spec) : SpecComp(spec) {
    std::vector<SanitzerSpec> specs = SanitzerSpec::extract(spec);
    for (auto &spec : specs) {
      if (spec.isRtSpec()) {
        rt_spec = spec;
      } else if (spec.isDepSpec()) {
        dep_spec = spec;
      }
    }
  }

  SanitzerSpec rt_spec;
  SanitzerSpec dep_spec;
};

class RoStrPatcher {
public:
  RoStrPatcher();
};

// Template of XSan rt spec
// However, it is too long to overwrite the original link spec,
// might lead to the unexpected overwrite of other strings.
// Therefore, this class is currently not used.
/// TODO: figure out how to delegate the runtime linking to the livepatch
/// completely.
class XsanRtSpecCompBuilder {
private:
  // Write %{xxx: ...}
  class ScopedWriter {
  public:
    ScopedWriter(std::string &spec, std::string_view hdr)
        : spec(spec), need_close(hdr.empty()) {
      if (hdr.substr(0, 2) != "%{") {
        spec.append("%{");
      }
      spec.append(hdr);
      if (hdr.back() != ':') {
        spec.push_back(':');
      }
    }
    ~ScopedWriter() {
      if (need_close)
        spec.push_back('}');
    }

  protected:
    bool need_close;

  private:
    std::string &spec;
  };

  class SanitizerScopedWriter : ScopedWriter {
  public:
    SanitizerScopedWriter(std::string &spec, SanitizerType type)
        : ScopedWriter(spec, getSanHdr(type)) {}

  private:
    std::string_view getSanHdr(SanitizerType type) {
      for (auto [ty, header] : Headers) {
        if (ty == type) {
          return header;
        }
      }
      return std::string_view();
    }
  };

public:
  static constexpr const char *Header = "%{%:sanitize(";
  using TyHdrPair = std::pair<SanitizerType, const char *>;
  static constexpr TyHdrPair Headers[] = {
      {ASan, "%{%:sanitize(address):"}, {TSan, "%{%:sanitize(thread):"},
      // {MSan, "%{%:sanitize(memory):"}, // Unsupported in GCC
      // {UBSan, "%{%:sanitize(undefined):"}, /// TODO: support UBSan?
  };

  /*
  %{%:sanitize(address):
      %{!shared:libasan_preinit%O%s}
      %{static-libasan:
          %{!shared:
          -Bstatic --whole-archive -lasan --no-whole-archive -Bdynamic}
      }
      %{!static-libasan:-lasan}
  }
  */
  XsanRtSpecCompBuilder(bool isCxx)
      : sanTy(getSanType()), xsan_mask(getXsanMask()), isCxx(isCxx) {
    if (sanTy == XSan && (!xsan_mask.test(ASan) || !xsan_mask.test(TSan))) {
      FATAL("We now only support XSan@ASan+TSan for GCC, as GCC does not "
            "support other sanitizers. Therefore, disactivate ASan or TSan "
            "from XSan seems meaningless for GCC.");
    }
    if (sanTy == XSan) {
      xsan_rt_prefix = getXsanArchRtDir() / "libclang_rt.xsan";
    } else if (sanTy == ASan) {
      xsan_rt_prefix = getXsanArchRtDir() / "libclang_rt.asan";
    } else if (sanTy == TSan) {
      xsan_rt_prefix = getXsanArchRtDir() / "libclang_rt.tsan";
    } else {
      // Nothing to do
      return;
    }
    build_full_spec();
  }

  std::string_view getRtSpec() const { return comp; }

private:
  /*
  %{fsanitize=address:
    %{fsanitize=thread:
      [XSan: @/path/to/xsan_wrapped_symbols.txt]
      %{!static-libasan:
        %{!static-libtsan:
          -rpath=/path/to/libclang_rt
          /path/to/libclang_rt.*san*.so
        }
      }
      # If not compile to DSO
      --whole-archive
      [ASan | XSan:
        %{!shared:/path/to/libclang_rt.*san-preinit.a}
        /path/to/libclang_rt.*san_static.a
      ]
      %{!shared:
        %{static-libasan:
          %{static-libtsan:
            /path/to/libclang_rt.*san.a
            [isCxx: /path/to/libclang_rt.*san_cxx.a]
          }
        }
      }
      --no-whole-archive
      %{!shared:
        %{static-libasan:
          %{static-libtsan:
            --dynamic-list=/path/to/libclang_rt.*san.a.syms
            [isCxx: --dynamic-list=/path/to/libclang_rt.*san_cxx.a.syms]
          }
        }
      }
    }
  }
  */
  void build_full_spec() {
    const std::bitset<NumSanitizerTypes> &xsan_mask = getXsanMask();
    SanitizerScopedWriter scope_asan(comp, xsan_mask[ASan] ? ASan : SanNone);
    SanitizerScopedWriter scope_tsan(comp, xsan_mask[TSan] ? TSan : SanNone);
    if (sanTy == XSan)
      add_wrap_link_option();
    {
      ScopedWriter nstatic_asan(comp, "!static-libasan");
      ScopedWriter nstatic_tsan(comp, "!static-libtsan");
      add_rpath();
      /// TODO: do not hardcode x86_64 here
      add_runtime("", "x86_64", "so");
    }

    comp.append(" --whole-archive");
    if (sanTy == XSan || sanTy == ASan) {
      {
        ScopedWriter nshared(comp, "!shared");
        add_runtime("-preinit", "x86_64", "a");
      }
      add_runtime("_static", "x86_64", "a");
    }
    {
      ScopedWriter nshared(comp, "!shared");
      ScopedWriter static_asan(comp, "static-libasan");
      ScopedWriter static_tsan(comp, "static-libtsan");
      add_runtime("", "x86_64", "a");
      if (isCxx) {
        add_runtime("_cxx", "x86_64", "a");
      }
    }
    comp.append(" --no-whole-archive");
    {
      ScopedWriter nshared(comp, "!shared");
      ScopedWriter static_asan(comp, "static-libasan");
      ScopedWriter static_tsan(comp, "static-libtsan");
      add_dynsyms("", "x86_64");
      if (isCxx) {
        add_dynsyms("_cxx", "x86_64");
      }
    }
  }

  void add_runtime(const char *suffix1, const char *arch, const char *suffix2,
                   bool isDynSyms = false) {
    comp.append(" " + xsan_rt_prefix + suffix1 + "-" + arch + "-" + suffix2);
  }
  void add_dynsyms(const char *suffix1, const char *arch) {
    comp.append(" --dynamic-list=" + xsan_rt_prefix + suffix1 + "-" + arch +
                ".a.syms");
  }
  void add_rpath() {
    static const std::string rPathOpt =
        "-rpath=" + getXsanArchRtDir().generic_string();
    comp.append(rPathOpt);
  }

  void add_wrap_link_option() {
    static const std::string WrapSymbolLinkOpt =
        "@" +
        (getXsanAbsPath(XSAN_SHARE_DIR "/" + getXsanCombName(getXsanMask())) /
         "xsan_wrapped_symbols.txt")
            .generic_string();
    comp.append(WrapSymbolLinkOpt);
  }

  // i.e., /path/to/lib/libclang_rt.xsan* or /path/to/lib/libclang_rt.asan*
  std::string xsan_rt_prefix;
  // The whole link spec of sanitizer runtime
  std::string comp;

  const SanitizerType sanTy;
  const std::bitset<NumSanitizerTypes> &xsan_mask;
  bool isCxx;
};

} // namespace

template <typename Ty> class A {
protected:
  using T = A<Ty>;
  static int a;
};

template <typename Ty> class B : public A<Ty> {
  void foo() { B::T::a = 1; }
  static int bar() { return B::a; }
};

RoStrPatcher::RoStrPatcher() {
  // Early exit unless XSan and livepatch are both enabled
  if (!isXsanEnabled() || !getenv(XSAN_LIVEPATCH_GCC_LINK_SPEC_FLAG))
    return;
  bool isCxx;
  // Match gcc, x86_64-linux-gnu-gcc-9 and etc.
  if (isPatchingProc("gcc"))
    isCxx = false;
  else if (isPatchingProc("g++"))
    isCxx = true;
  else
    return;

  const std::vector<ROSegment> &ro_sec = getSelfModuleROSegments();
  std::string_view spec = find_link_command_spec(ro_sec);
  if (spec.empty()) {
    return;
  }
  LinkSpec link_spec(spec);
  NewLinkSpecBuilder builder(spec);

  // ------ Modify the link spec ------
  const std::bitset<NumSanitizerTypes> &xsan_mask = getXsanMask();
  /// TODO: should we delegate UBSan? UBSan's linking is in dep_spec.
  static constexpr std::pair<SanitizerType, const char *> MaskedSan[] = {
      {ASan, "ASan"},
      {TSan, "TSan"},
  };
  for (auto [san, name] : MaskedSan) {
    if (xsan_mask.test(san)) {
      auto comp = link_spec.rt_spec.getSanitizerComp(san);
      if (!comp) {
        FATAL("%s's runtime is not found in the link spec", name);
      }
      // Remove the sanitizer runtime from the link spec
      comp->remove(builder);
    }
  }

  // ------ Modify the link spec ------
  std::string new_spec = builder.build();
  memcpy_forcibly(const_cast<char *>(spec.data()), new_spec.c_str(),
                  new_spec.size() + 1 /* Copy the tailing '\0' */, false);
}

RoStrPatcher str_patcher;
