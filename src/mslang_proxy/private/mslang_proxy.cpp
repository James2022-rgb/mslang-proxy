// TU header --------------------------------------------
#include "mslang_proxy/public/mslang_proxy.h"

// public project headers -------------------------------
#include "mbase/public/platform.h"

#if !MBASE_PLATFORM_WEB

namespace mslang_proxy {

std::optional<std::vector<uint32_t>> CompileSlangToSpirv(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* capability_name,
  char const* entry_point_name
) {
  return mslang::CompileSlangToSpirv(
    module_name,
    path,
    slang_code,
    opt_include_handler,
    capability_name,
    entry_point_name
  );
}

std::optional<std::string> CompileSlangToWgsl(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* entry_point_name
) {
  return mslang::CompileSlangToWgsl(
    module_name,
    path,
    slang_code,
    opt_include_handler,
    entry_point_name
  );
}

} // namespace mslang_proxy

#endif // !MBASE_PLATFORM_WEB
