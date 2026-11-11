cmake_minimum_required(VERSION 3.24)
project(gmod_mcp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Static CRT everywhere (module and dependencies): no redistributable to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Only 64-bit targets are supported")
endif()
if(WIN32)
  set(MCP_PLATFORM win64)
elseif(APPLE)
  set(MCP_PLATFORM osx64)
else()
  set(MCP_PLATFORM linux64)
endif()

# --- Dependencies: downloaded automatically at configure time, pinned versions verified by SHA-256 ---
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# cpp-httplib (MIT) - header only: nonexistent SOURCE_SUBDIR so that its CMakeLists is not included.
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.65.0.tar.gz
  URL_HASH SHA256=6fda8f23ff811a751c2b6e694fe2f1aa1625df53a317110329ef57a18f9fa5fb
  SOURCE_SUBDIR _header_only_)

# nlohmann/json (MIT).
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)

# json-schema-validator (MIT) - validation of MCP tool inputs.
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json_schema_validator
  URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.4.1.tar.gz
  URL_HASH SHA256=0c8bf7ba59bd50e312d82da4bab1d49cf3e2211cfbd659472f9c90505d2a4e4c)

# garrysmod_common (BSD-3-Clause) - only the LuaShared.h header is used.
FetchContent_Declare(garrysmod_common
  URL https://github.com/danielga/garrysmod_common/archive/f77a18d86f780a59ea30e4237016b05b790d4b70.tar.gz
  URL_HASH SHA256=ec74b1b2c29f8b9159e917f9d0c6239cd2334680c232270dcd16e109383cf2ef
  SOURCE_SUBDIR _header_only_)

FetchContent_MakeAvailable(httplib nlohmann_json json_schema_validator garrysmod_common)

set(PLATFORM_SOURCES src/platform/platform_win.cpp src/platform/platform_posix.cpp)
# One version for the module and the bridge (the bridge reports a module of another version).
add_compile_definitions(GMOD_MCP_VERSION="${PROJECT_VERSION}")

  # Embedded Lua runtime: each src/lua/*.lua file becomes a byte array in embedded_lua.h.
  file(GLOB LUA_RUNTIME_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/lua/*.lua)
  set(EMBED_OUT ${CMAKE_BINARY_DIR}/generated/embedded_lua.h)
  add_custom_command(
    OUTPUT ${EMBED_OUT}
    COMMAND ${CMAKE_COMMAND} -DOUT=${EMBED_OUT} -DSRC_DIR=${CMAKE_SOURCE_DIR}/src/lua -P ${CMAKE_SOURCE_DIR}/cmake/embed_lua.cmake
    DEPENDS ${LUA_RUNTIME_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_lua.cmake
    COMMENT "Embedding the Lua runtime")
  file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
  add_library(mcpagent SHARED ${CORE_SOURCES} ${PLATFORM_SOURCES} ${EMBED_OUT})
  target_include_directories(mcpagent PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_link_libraries(mcpagent PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator)

target_include_directories(mcpagent PRIVATE src ${httplib_SOURCE_DIR} ${garrysmod_common_SOURCE_DIR}/include)

set_target_properties(mcpagent PROPERTIES
  PREFIX ""
  OUTPUT_NAME "gmsv_mcpagent_${MCP_PLATFORM}"
  SUFFIX ".dll")

if(MSVC)
  target_compile_options(mcpagent PRIVATE /W4 /utf-8 /EHsc /permissive- /wd4100 /wd4996)
  target_compile_definitions(mcpagent PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(mcpagent PRIVATE ws2_32 bcrypt)
else()
  target_compile_options(mcpagent PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fvisibility=hidden)
  target_link_libraries(mcpagent PRIVATE dl pthread -static-libstdc++ -static-libgcc)
endif()

# --- Offline snapshot: tools, prompts, resources and instructions built from the module's sources without the game,
# embedded into the bridge (served while the game has never answered). The generator is a build tool, not shipped.
add_executable(gmod-mcp-snapshot src/snapshot/snapshot_main.cpp ${CORE_SOURCES} ${PLATFORM_SOURCES} ${EMBED_OUT})
target_include_directories(gmod-mcp-snapshot PRIVATE src ${CMAKE_BINARY_DIR}/generated ${httplib_SOURCE_DIR} ${garrysmod_common_SOURCE_DIR}/include)
target_link_libraries(gmod-mcp-snapshot PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator)
if(MSVC)
  target_compile_options(gmod-mcp-snapshot PRIVATE /utf-8 /EHsc /permissive- /wd4100 /wd4996)
  target_compile_definitions(gmod-mcp-snapshot PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(gmod-mcp-snapshot PRIVATE ws2_32 bcrypt iphlpapi user32)
else()
  target_link_libraries(gmod-mcp-snapshot PRIVATE dl pthread)
endif()
set(SNAPSHOT_JSON ${CMAKE_BINARY_DIR}/generated/bridge_snapshot.json)
set(SNAPSHOT_H ${CMAKE_BINARY_DIR}/generated/bridge_snapshot.h)
add_custom_command(
  OUTPUT ${SNAPSHOT_H}
  COMMAND gmod-mcp-snapshot ${SNAPSHOT_JSON}
  COMMAND ${CMAKE_COMMAND} -DIN=${SNAPSHOT_JSON} -DOUT=${SNAPSHOT_H} -DNAME=bridge_snapshot -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS gmod-mcp-snapshot ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  COMMENT "Building the bridge's offline snapshot")

# --- gmod-mcp-bridge: stdio MCP server started by the MCP client, relays to the module (works while the game is closed) ---
add_executable(gmod-mcp-bridge src/bridge/bridge.cpp src/util.cpp ${PLATFORM_SOURCES} ${SNAPSHOT_H})
target_include_directories(gmod-mcp-bridge PRIVATE src ${httplib_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(gmod-mcp-bridge PRIVATE nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(gmod-mcp-bridge PRIVATE /W4 /utf-8 /EHsc /permissive- /wd4100 /wd4996)
  target_compile_definitions(gmod-mcp-bridge PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(gmod-mcp-bridge PRIVATE ws2_32 bcrypt iphlpapi user32)
else()
  target_compile_options(gmod-mcp-bridge PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(gmod-mcp-bridge PRIVATE pthread -static-libstdc++ -static-libgcc)
endif()
