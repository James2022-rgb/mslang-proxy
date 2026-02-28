## Overview

`mslang-proxy` is a proxy layer for Slang shader compilation.

- **Native**: directly forwards to `mslang::CompileSlangToSpirv` / `CompileSlangToWgsl`.
- **Emscripten**: sends compilation requests to a local WebSocket server (`/ws/compile`), avoiding having to embed the Slang compiler in WASM.

Namespace: `mslang_proxy`. Types (`SlangIncludeHandler`, `SlangIncludeResult`, etc.) are reused from `mslang` (header-only dependency).

## Language

All code comments MUST be written in English.

## Symbols

It is forbidden to use the full width forms of symbols that have counterparts in ASCII.

e.g. `()`, `:`, `,`, `0-9`

## Coding style

Follow the wentos project `STYLE_GUIDE.md`.

## Commit messages

Follow Conventional Commits. See `.github/git-commit-instructions.md`.

## Directory structure

```
src/mslang_proxy/
  public/
    mslang_proxy.h                  Public header (mslang_proxy namespace)
  private/
    mslang_proxy.cpp                Native implementation (#if !MBASE_PLATFORM_WEB)
    mslang_proxy_emscripten.cpp     Emscripten WebSocket impl (#if MBASE_PLATFORM_WEB)
```

## Dependencies

- `mbase` - Logging, platform detection. Always linked PUBLIC.
- `mslang` - Slang compiler wrapper. Linked PUBLIC on native only. On Emscripten, only the public headers are used (for type definitions).

## Build

Static library (`mslang-proxy`). C++23. Used as a git submodule in `wentos`.

- Native: links `mslang` (full Slang compiler).
- Emscripten: does NOT link `mslang`. Links `-lwebsocket.js` for the Emscripten WebSocket API.

## WebSocket protocol (Emscripten)

Binary messages. First byte = message type:

| Direction | Type   | Payload                                                    |
|-----------|--------|------------------------------------------------------------|
| C->S      | `0x01` | compile: JSON (module_name, path, slang_code, ...)         |
| S->C      | `0x02` | resolve_include: UTF-8 path string                         |
| C->S      | `0x03` | include_result: success(1) + path_len(4 LE) + path + bytes |
| S->C      | `0x04` | compile_done: raw SPIR-V or WGSL bytes                     |
| S->C      | `0x05` | compile_error: UTF-8 error message                         |

## Key implementation notes

- `emscripten_sleep()` yields to the browser event loop, allowing WebSocket callbacks to fire during the blocking wait.
- Include handler callbacks may perform nested ASYNCIFY operations (e.g. `LoadAssetEx`).
- WebSocket URL is derived from `window.location.host` (same-origin, local dev only).
