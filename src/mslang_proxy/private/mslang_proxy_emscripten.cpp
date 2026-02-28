// TU header --------------------------------------------
#include "mslang_proxy/public/mslang_proxy.h"

// public project headers -------------------------------
#include "mbase/public/log.h"
#include "mbase/public/platform.h"

#if MBASE_PLATFORM_WEB

// c++ headers ------------------------------------------
#include <atomic>
#include <cstring>

// external headers -------------------------------------
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

namespace mslang_proxy {

namespace {

// Simple JSON string escaping for shader source code.
std::string EscapeJsonString(char const* s) {
  std::string out;
  out.reserve(std::strlen(s) + 64);
  for (char const* p = s; *p != '\0'; ++p) {
    switch (*p) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(*p) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(*p));
          out += buf;
        } else {
          out += *p;
        }
        break;
    }
  }
  return out;
}

struct CompileContext {
  std::atomic<bool> done{false};
  std::atomic<bool> success{false};
  std::vector<std::byte> response_data;

  // Include handler support.
  mslang::SlangIncludeHandler include_handler;
  std::string pending_include_path;
  bool include_requested{false};
};

EM_BOOL OnWebSocketMessage(
  int /*event_type*/,
  EmscriptenWebSocketMessageEvent const* event,
  void* user_data
) {
  auto* ctx = static_cast<CompileContext*>(user_data);

  if (event->numBytes < 1) {
    return EM_TRUE;
  }

  uint8_t const msg_type = event->data[0];

  switch (msg_type) {
    case 0x02: {
      // resolve_include: server requests include resolution.
      std::string path(
        reinterpret_cast<char const*>(event->data + 1),
        event->numBytes - 1
      );
      ctx->pending_include_path = std::move(path);
      ctx->include_requested = true;
      break;
    }

    case 0x04: {
      // compile_done: SPIR-V or WGSL bytes.
      ctx->response_data.resize(event->numBytes - 1);
      std::memcpy(ctx->response_data.data(), event->data + 1, event->numBytes - 1);
      ctx->success = true;
      ctx->done = true;
      break;
    }

    case 0x05: {
      // compile_error: UTF-8 error message.
      std::string error_msg(
        reinterpret_cast<char const*>(event->data + 1),
        event->numBytes - 1
      );
      MBASE_LOG_ERROR("CompileSlang (proxy): server error: {}", error_msg);
      ctx->success = false;
      ctx->done = true;
      break;
    }

    default:
      MBASE_LOG_WARN("CompileSlang (proxy): unknown message type 0x{:02X}", msg_type);
      break;
  }

  return EM_TRUE;
}

EM_BOOL OnWebSocketError(
  int /*event_type*/,
  EmscriptenWebSocketErrorEvent const* /*event*/,
  void* user_data
) {
  auto* ctx = static_cast<CompileContext*>(user_data);
  MBASE_LOG_ERROR("CompileSlang (proxy): WebSocket error");
  ctx->success = false;
  ctx->done = true;
  return EM_TRUE;
}

EM_BOOL OnWebSocketClose(
  int /*event_type*/,
  EmscriptenWebSocketCloseEvent const* event,
  void* user_data
) {
  auto* ctx = static_cast<CompileContext*>(user_data);
  if (!ctx->done) {
    MBASE_LOG_ERROR("CompileSlang (proxy): WebSocket closed unexpectedly (code={})",
      event->code);
    ctx->success = false;
    ctx->done = true;
  }
  return EM_TRUE;
}

// Send include result back to server.
// Protocol: 0x03 + success(1) + resolved_path_len(4, LE) + resolved_path + bytes
void SendIncludeResult(
  EMSCRIPTEN_WEBSOCKET_T ws,
  std::optional<mslang::SlangIncludeResult> const& result
) {
  if (!result.has_value()) {
    // Send failure.
    uint8_t msg[2] = {0x03, 0x00};
    emscripten_websocket_send_binary(ws, msg, sizeof(msg));
    return;
  }

  auto const& r = result.value();
  uint32_t const path_len = static_cast<uint32_t>(r.resolved_path.size());
  size_t const total_size = 1 + 1 + 4 + path_len + r.bytes.size();

  std::vector<uint8_t> msg(total_size);
  size_t offset = 0;

  msg[offset++] = 0x03;               // message type
  msg[offset++] = 0x01;               // success
  std::memcpy(&msg[offset], &path_len, 4); offset += 4;  // path length (LE)
  std::memcpy(&msg[offset], r.resolved_path.data(), path_len); offset += path_len;
  std::memcpy(&msg[offset], r.bytes.data(), r.bytes.size());

  emscripten_websocket_send_binary(ws, msg.data(), static_cast<uint32_t>(msg.size()));
}

std::optional<std::vector<std::byte>> CompileViaWebSocket(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* capability_name,
  char const* entry_point_name,
  char const* target
) {
  // Build WebSocket URL from current page origin.
  char* ws_url = static_cast<char*>(EM_ASM_PTR({
    var s = 'ws://' + window.location.host + '/ws/compile';
    var len = lengthBytesUTF8(s) + 1;
    var buf = _malloc(len);
    stringToUTF8(s, buf, len);
    return buf;
  }));

  EmscriptenWebSocketCreateAttributes ws_attrs = {
    ws_url,
    nullptr,
    EM_TRUE
  };

  EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&ws_attrs);
  free(ws_url);

  if (ws <= 0) {
    MBASE_LOG_ERROR("CompileSlang (proxy): failed to create WebSocket");
    return std::nullopt;
  }

  CompileContext ctx;
  ctx.include_handler = opt_include_handler;

  emscripten_websocket_set_onmessage_callback(ws, &ctx, OnWebSocketMessage);
  emscripten_websocket_set_onerror_callback(ws, &ctx, OnWebSocketError);
  emscripten_websocket_set_onclose_callback(ws, &ctx, OnWebSocketClose);

  // Wait for connection to open.
  std::atomic<bool> connected{false};
  emscripten_websocket_set_onopen_callback(ws, &connected,
    [](int, EmscriptenWebSocketOpenEvent const*, void* ud) -> EM_BOOL {
      auto* flag = static_cast<std::atomic<bool>*>(ud);
      *flag = true;
      return EM_TRUE;
    }
  );

  while (!connected && !ctx.done) {
    emscripten_sleep(10);
  }

  if (ctx.done) {
    emscripten_websocket_delete(ws);
    return std::nullopt;
  }

  // Build and send compile request (0x01 + JSON).
  {
    std::string json;
    json += "{";
    json += "\"module_name\":\"" + EscapeJsonString(module_name) + "\",";
    json += "\"path\":\"" + EscapeJsonString(path) + "\",";
    json += "\"slang_code\":\"" + EscapeJsonString(slang_code) + "\",";
    if (capability_name) {
      json += "\"capability_name\":\"" + EscapeJsonString(capability_name) + "\",";
    }
    json += "\"entry_point_name\":\"" + EscapeJsonString(entry_point_name) + "\",";
    json += "\"target\":\"" + std::string(target) + "\"";
    json += "}";

    std::vector<uint8_t> msg(1 + json.size());
    msg[0] = 0x01;
    std::memcpy(msg.data() + 1, json.data(), json.size());

    emscripten_websocket_send_binary(ws, msg.data(), static_cast<uint32_t>(msg.size()));
  }

  // Wait for completion, handling include requests.
  while (!ctx.done) {
    emscripten_sleep(10);

    if (ctx.include_requested) {
      ctx.include_requested = false;
      std::string include_path = std::move(ctx.pending_include_path);

      if (ctx.include_handler) {
        // Call the user's include handler (may perform ASYNCIFY operations).
        auto result = ctx.include_handler(include_path.c_str());
        SendIncludeResult(ws, result);
      } else {
        MBASE_LOG_WARN("CompileSlang (proxy): server requested include \"{}\" "
          "but no include handler is set", include_path);
        SendIncludeResult(ws, std::nullopt);
      }
    }
  }

  emscripten_websocket_close(ws, 1000, "done");
  emscripten_websocket_delete(ws);

  if (!ctx.success) {
    return std::nullopt;
  }

  return std::move(ctx.response_data);
}

} // anonymous namespace

std::optional<std::vector<uint32_t>> CompileSlangToSpirv(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* capability_name,
  char const* entry_point_name
) {
  auto result = CompileViaWebSocket(
    module_name, path, slang_code, opt_include_handler,
    capability_name, entry_point_name, "spirv"
  );

  if (!result.has_value()) {
    return std::nullopt;
  }

  auto const& bytes = result.value();
  if (bytes.size() % 4 != 0) {
    MBASE_LOG_ERROR("CompileSlangToSpirv (proxy): response size {} is not "
      "a multiple of 4", bytes.size());
    return std::nullopt;
  }

  std::vector<uint32_t> spirv(bytes.size() / 4);
  std::memcpy(spirv.data(), bytes.data(), bytes.size());
  return spirv;
}

std::optional<std::string> CompileSlangToWgsl(
  char const* module_name,
  char const* path,
  char const* slang_code,
  mslang::SlangIncludeHandler opt_include_handler,
  char const* entry_point_name
) {
  auto result = CompileViaWebSocket(
    module_name, path, slang_code, opt_include_handler,
    nullptr, entry_point_name, "wgsl"
  );

  if (!result.has_value()) {
    return std::nullopt;
  }

  auto const& bytes = result.value();
  return std::string(reinterpret_cast<char const*>(bytes.data()), bytes.size());
}

} // namespace mslang_proxy

#endif // MBASE_PLATFORM_WEB
