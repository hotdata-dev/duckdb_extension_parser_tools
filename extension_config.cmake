# This file is included by DuckDB's build system. It specifies which extension to load

# DuckDB v1.4.4's bundled fmt uses stdext::checked_array_iterator when
# _SECURE_SCL is defined. VS 2026 removed that non-standard iterator, so select
# fmt's portable pointer implementation before DuckDB configures third_party.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
include(${CMAKE_CURRENT_LIST_DIR}/scripts/patch_bundled_fmt.cmake)

# Extension from this repo
duckdb_extension_load(parser_tools
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
