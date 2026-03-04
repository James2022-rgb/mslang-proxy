// TU header --------------------------------------------
#include "mslang_proxy/public/mslang_proxy.h"

// c++ headers ------------------------------------------
#include <filesystem>
#include <fstream>

// public project headers -------------------------------
#include "mbase/public/log.h"
#include "mbase/public/platform.h"

#include "mslang/public/slang_cache.h"

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

// ----------------------------------------------------------------------------------------------------
// Slang module cache
//

static std::string s_spv_cache_dir;

void InitializeSlangCache(std::string_view cache_parent_path) {
  mslang::ISlangCache::InitializeGlobal(cache_parent_path);
  s_spv_cache_dir = (std::filesystem::path(cache_parent_path) / "slang_cache").string();
}

// ----------------------------------------------------------------------------------------------------
// SPIR-V batch cache
//
// Caches compiled SPIR-V per entry point so that subsequent runs skip the Slang
// session entirely. The cache file stores dependency timestamps for validation.
//
// Binary format:
//   [4] magic "SPVB"
//   [8] root_module_timestamp
//   [4] dependency_count
//   For each dependency:
//     [4] resolved_path_length
//     [N] resolved_path
//     [8] timestamp
//   [4] entry_point_count
//   For each entry point:
//     [4] name_length
//     [N] name
//     [4] spirv_word_count
//     [N*4] spirv_data
//

namespace {

constexpr uint32_t kSpvBatchMagic = 0x42565053; // "SPVB"

struct SpvBatchDependency {
  std::string resolved_path;
  uint64_t timestamp;
};

struct SpvBatchEntryPoint {
  std::string name;
  std::vector<uint32_t> spirv;
};

struct SpvBatchCache {
  uint64_t root_module_timestamp;
  std::vector<SpvBatchDependency> dependencies;
  std::vector<SpvBatchEntryPoint> entry_points;
};

template<typename T>
bool BinRead(std::ifstream& f, T& v) {
  return !!f.read(reinterpret_cast<char*>(&v), sizeof(T));
}

template<typename T>
void BinWrite(std::ofstream& f, T const& v) {
  f.write(reinterpret_cast<char const*>(&v), sizeof(T));
}

bool BinReadString(std::ifstream& f, std::string& s) {
  uint32_t len;
  if (!BinRead(f, len)) return false;
  s.resize(len);
  return !!f.read(s.data(), len);
}

void BinWriteString(std::ofstream& f, std::string const& s) {
  uint32_t len = static_cast<uint32_t>(s.size());
  BinWrite(f, len);
  f.write(s.data(), len);
}

bool LoadSpvBatchCache(std::filesystem::path const& path, SpvBatchCache& cache) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;

  uint32_t magic;
  if (!BinRead(f, magic) || magic != kSpvBatchMagic) return false;
  if (!BinRead(f, cache.root_module_timestamp)) return false;

  uint32_t dep_count;
  if (!BinRead(f, dep_count)) return false;
  cache.dependencies.resize(dep_count);
  for (uint32_t i = 0; i < dep_count; ++i) {
    if (!BinReadString(f, cache.dependencies[i].resolved_path)) return false;
    if (!BinRead(f, cache.dependencies[i].timestamp)) return false;
  }

  uint32_t ep_count;
  if (!BinRead(f, ep_count)) return false;
  cache.entry_points.resize(ep_count);
  for (uint32_t i = 0; i < ep_count; ++i) {
    if (!BinReadString(f, cache.entry_points[i].name)) return false;
    uint32_t word_count;
    if (!BinRead(f, word_count)) return false;
    cache.entry_points[i].spirv.resize(word_count);
    if (!f.read(reinterpret_cast<char*>(cache.entry_points[i].spirv.data()),
                word_count * sizeof(uint32_t))) {
      return false;
    }
  }

  return true;
}

void SaveSpvBatchCache(std::filesystem::path const& path, SpvBatchCache const& cache) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return;

  BinWrite(f, kSpvBatchMagic);
  BinWrite(f, cache.root_module_timestamp);

  BinWrite(f, static_cast<uint32_t>(cache.dependencies.size()));
  for (auto const& dep : cache.dependencies) {
    BinWriteString(f, dep.resolved_path);
    BinWrite(f, dep.timestamp);
  }

  BinWrite(f, static_cast<uint32_t>(cache.entry_points.size()));
  for (auto const& ep : cache.entry_points) {
    BinWriteString(f, ep.name);
    BinWrite(f, static_cast<uint32_t>(ep.spirv.size()));
    f.write(reinterpret_cast<char const*>(ep.spirv.data()),
            ep.spirv.size() * sizeof(uint32_t));
  }
}

bool ValidateSpvBatchCache(
  SpvBatchCache const& cache,
  char const* root_module_parent_path,
  char const* root_module_name,
  mslang::ISlangCodeProvider* code_provider,
  std::span<char const* const> entry_point_names
) {
  // Entry point count and names must match exactly.
  if (cache.entry_points.size() != entry_point_names.size()) return false;
  for (size_t i = 0; i < entry_point_names.size(); ++i) {
    if (cache.entry_points[i].name != entry_point_names[i]) return false;
  }

  // Root module timestamp.
  uint64_t current_root_ts = 0;
  if (!code_provider->ProvideSlangCode(
        root_module_parent_path, root_module_name, nullptr, current_root_ts)) {
    return false;
  }
  if (cache.root_module_timestamp != current_root_ts) return false;

  // Dependency timestamps.
  for (auto const& dep : cache.dependencies) {
    uint64_t current_dep_ts = 0;
    if (!code_provider->ProvideSlangCodeTimestampResolvedPath(
          dep.resolved_path, current_dep_ts)) {
      return false;
    }
    if (dep.timestamp != current_dep_ts) return false;
  }

  return true;
}

} // anonymous namespace

CompileBatchResult CompileSlangToSpirvBatch(
  char const* root_module_parent_path,
  char const* root_module_name,
  mslang::ISlangCodeProvider* code_provider,
  mslang::ISlangDependencyIncludeHandler* include_handler,
  char const* capability_name,
  std::span<char const* const> entry_point_names
) {
  CompileBatchResult result;
  result.spirv.resize(entry_point_names.size());

  mslang::ISlangCache* cache = mslang::ISlangCache::GetGlobal();

  // Try SPIR-V batch cache: skip Slang session entirely if all timestamps match.
  std::filesystem::path spv_cache_path;
  if (cache && !s_spv_cache_dir.empty()) {
    spv_cache_path = std::filesystem::path(s_spv_cache_dir) /
      (std::string(root_module_name) + ".spv-batch");

    SpvBatchCache batch_cache;
    if (LoadSpvBatchCache(spv_cache_path, batch_cache) &&
        ValidateSpvBatchCache(batch_cache, root_module_parent_path,
                              root_module_name, code_provider, entry_point_names)) {
      MBASE_LOG_INFO("CompileSlangToSpirvBatch: SPIR-V cache hit for \"{}\"", root_module_name);
      for (size_t i = 0; i < entry_point_names.size(); ++i) {
        result.spirv[i] = std::move(batch_cache.entry_points[i].spirv);
      }
      return result;
    }
  }

  // SPIR-V cache miss: compile via Slang session.
  mslang::ISlangSession* session = nullptr;

  if (cache) {
    mslang::SlangSessionWrmKey key {
      .root_module_parent_path = root_module_parent_path,
      .root_module_name = root_module_name,
    };
    if (!cache->RequestSlangSessionWithRootModule(
          key, nullptr, code_provider, include_handler, &session)) {
      MBASE_LOG_ERROR("CompileSlangToSpirvBatch: Failed to get cached session for \"{}\"", root_module_name);
      return result;
    }
  } else {
    mslang::SlangIncludeHandler handler =
      [include_handler](char const* path)
        -> std::optional<mslang::SlangIncludeResult> {
      mslang::SlangIncludeResult r;
      if (!include_handler->HandleInclude(path, r)) return std::nullopt;
      return r;
    };

    session = mslang::ISlangSession::Create(handler, capability_name);

    std::string code;
    uint64_t ts = 0;
    code_provider->ProvideSlangCode(
      root_module_parent_path, root_module_name, &code, ts);

    session->AddModuleFromCode(
      root_module_name, root_module_parent_path, code.c_str(), nullptr);
  }

  for (size_t i = 0; i < entry_point_names.size(); ++i) {
    result.spirv[i] = session->CompileModuleEntryPointToSpirv(
      nullptr, entry_point_names[i]);
  }

  // Save SPIR-V batch cache for next run.
  if (!spv_cache_path.empty()) {
    bool all_compiled = true;
    for (size_t i = 0; i < entry_point_names.size(); ++i) {
      if (!result.spirv[i].has_value()) {
        all_compiled = false;
        break;
      }
    }

    if (all_compiled) {
      SpvBatchCache batch_cache;

      uint64_t root_ts = 0;
      code_provider->ProvideSlangCode(
        root_module_parent_path, root_module_name, nullptr, root_ts);
      batch_cache.root_module_timestamp = root_ts;

      std::span<mslang::SlangModuleDependency const> deps =
        session->GetModuleDependencies(nullptr);
      for (auto const& dep : deps) {
        batch_cache.dependencies.push_back({dep.resolved_path, dep.timestamp});
      }

      for (size_t i = 0; i < entry_point_names.size(); ++i) {
        batch_cache.entry_points.push_back({
          std::string(entry_point_names[i]),
          result.spirv[i].value(),
        });
      }

      SaveSpvBatchCache(spv_cache_path, batch_cache);
      MBASE_LOG_INFO("CompileSlangToSpirvBatch: SPIR-V cache saved for \"{}\"", root_module_name);
    }
  }

  session->Destroy();
  return result;
}

} // namespace mslang_proxy

#endif // !MBASE_PLATFORM_WEB
