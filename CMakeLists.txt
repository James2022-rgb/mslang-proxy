CMAKE_MINIMUM_REQUIRED(VERSION 3.22)

SET(CMAKE_CXX_STANDARD 23)
SET(CMAKE_CXX_STANDARD_REQUIRED True)

set(TARGET_NAME "mslang-proxy")
set(SRC_DIR "src")

project(${TARGET_NAME}
  VERSION 0.1.0.0
  LANGUAGES CXX
)

add_library(${TARGET_NAME} STATIC)

target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

# Compiler warnings.
if(MSVC)
  target_compile_options(${TARGET_NAME} PRIVATE /W4)
else()
  target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Werror)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${TARGET_NAME} PRIVATE -Werror=thread-safety)
  endif()
endif()

# Sources.
set(SOURCES_PUBLIC
  ${SRC_DIR}/mslang_proxy/public/mslang_proxy.h
)

set(SOURCES_PRIVATE
  ${SRC_DIR}/mslang_proxy/private/mslang_proxy.cpp
  ${SRC_DIR}/mslang_proxy/private/mslang_proxy_emscripten.cpp
)

source_group("Public" FILES ${SOURCES_PUBLIC})
source_group("Private" FILES ${SOURCES_PRIVATE})

target_sources(${TARGET_NAME} PRIVATE
  ${SOURCES_PUBLIC}
  ${SOURCES_PRIVATE}
)

# Include directories.
target_include_directories(${TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SRC_DIR}/mslang_proxy/private
)

target_include_directories(${TARGET_NAME} PUBLIC
  ${SRC_DIR}
)

# Dependencies.
target_link_libraries(${TARGET_NAME} PUBLIC
  mbase
)

if(NOT EMSCRIPTEN)
  # Native: link mslang to forward calls.
  target_link_libraries(${TARGET_NAME} PUBLIC
    mslang
  )
else()
  # Emscripten: mslang is not built. Provide header-only include path
  # for type definitions (SlangIncludeHandler, SlangIncludeResult, etc.).
  target_include_directories(${TARGET_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../mslang/src
  )

  # Emscripten WebSocket library for the proxy implementation.
  target_link_options(${TARGET_NAME} PUBLIC -lwebsocket.js)
endif()
