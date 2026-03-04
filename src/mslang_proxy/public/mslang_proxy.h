#pragma once

// c++ headers ------------------------------------------
#include <cstdint>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// public project headers -------------------------------
#include "mslang/public/mslang-slang.h"

// Forward declarations for ISlangCache support.
namespace mslang {
class ISlangCodeProvider;
class ISlangDependencyIncludeHandler;
}

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

// ----------------------------------------------------------------------------------------------------
// Slang module cache
//

/// Initialize the Slang module cache.
/// On native: calls mslang::ISlangCache::InitializeGlobal().
/// On Emscripten: no-op.
void InitializeSlangCache(std::string_view cache_parent_path);

struct CompileBatchResult {
  /// One entry per requested entry point. nullopt = compilation failed.
  std::vector<std::optional<std::vector<uint32_t>>> spirv;
};

/// Compile multiple entry points from a single Slang module.
///
/// On native with cache initialized: uses ISlangCache to cache the compiled
/// module across startups. The Slang module is compiled only on first run
/// (or when sources change); subsequent runs load from cache.
///
/// On native without cache: creates one session, compiles module once,
/// generates SPIR-V for each entry point.
///
/// On Emscripten: falls back to individual CompileSlangToSpirv calls.
CompileBatchResult CompileSlangToSpirvBatch(
  char const* root_module_parent_path,
  char const* root_module_name,
  mslang::ISlangCodeProvider* code_provider,
  mslang::ISlangDependencyIncludeHandler* include_handler,
  char const* capability_name,
  std::span<char const* const> entry_point_names
);

} // namespace mslang_proxy
