PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=parser_tools
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# duckdb/CMakeLists.txt sets CMAKE_CXX_STANDARD 11 as a plain (non-FORCE) cache
# default; MSVC has no /std:c++11, so on real MSVC that resolves to its
# implicit default dialect, which does not enable C++17 inline variables. The
# vendored fmt (third_party/fmt) needs them, so a real-MSVC windows_amd64
# build fails compiling fmt's format.h ("inline variables require
# /std:c++17"). Passing this on the cmake command line wins over the
# unforced cache set() in duckdb's own CMakeLists.txt. Scoped to plain MSVC:
# windows_amd64_mingw/_rtools build with g++, which already compiles fmt
# fine under GCC's default dialect.
ifeq ($(DUCKDB_PLATFORM),windows_amd64)
	BUILD_FLAGS += -DCMAKE_CXX_STANDARD=17
endif