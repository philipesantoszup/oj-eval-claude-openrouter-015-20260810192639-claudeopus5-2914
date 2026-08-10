cmake_minimum_required(VERSION 3.10)
project(code CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_executable(code main.cpp)

# The memory limit for this problem is tight enough that the runtime linker and
# the shared libc mappings matter.  Link statically when the toolchain supports
# it, and fall back silently when it does not.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-static")
check_cxx_source_compiles("int main() { return 0; }" HAVE_STATIC_LINK)
unset(CMAKE_REQUIRED_FLAGS)

if(HAVE_STATIC_LINK)
  set_target_properties(code PROPERTIES LINK_FLAGS "-static")
endif()
