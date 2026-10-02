cmake_minimum_required(VERSION 3.20)
project(vamiga_lua C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The vAmiga core, from the Silicium repository. Its build files add include
# directories to the Silicium application target, which is not built here;
# this library stands in for it.
add_library(silicium STATIC src/silicium_stub.cpp)
add_subdirectory(extern/silicium/Cores cores EXCLUDE_FROM_ALL)

# Lua.
file(GLOB LUA_SOURCES extern/lua/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC extern/lua)
target_compile_definitions(lua PRIVATE LUA_USE_POSIX)

find_package(ZLIB REQUIRED)

add_executable(vamiga-lua
  src/engine.cpp
  src/json.cpp
  src/lua_cpu.cpp
  src/lua_input.cpp
  src/lua_mem.cpp
  src/lua_state.cpp
  src/lua_video.cpp
  src/main.cpp
  src/remote.cpp
  src/sockets.cpp)
target_link_libraries(vamiga-lua VACore lua ZLIB::ZLIB)
