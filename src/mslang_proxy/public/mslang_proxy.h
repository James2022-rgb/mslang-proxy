#pragma once

// c++ headers ------------------------------------------
#include <cstdint>

#include <optional>
#include <string>
#include <vector>

// public project headers -------------------------------
#include "mslang/public/mslang-slang.h"

namespace mslang_proxy {

/// Compile Slang source code to SPIR-V.
/// On native platforms, delegates to mslang::CompileSlangToSpirv.
/// On Emscripten, sends the compilation request to a server via WebSocket.
std::optional<std::vector<uint32_t>> CompileSlangToSpirv(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* capability_name,
  char const* entry_point_name
);

/// Compile Slang source code to WGSL.
/// On native platforms, delegates to mslang::CompileSlangToWgsl.
/// On Emscripten, sends the compilation request to a server via WebSocket.
std::optional<std::string> CompileSlangToWgsl(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* entry_point_name
);

} // namespace mslang_proxy
