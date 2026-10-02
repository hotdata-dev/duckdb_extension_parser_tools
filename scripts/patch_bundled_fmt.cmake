# Work around fmt 6.1.2 using the removed MSVC stdext::checked_array_iterator.
#
# This is intentionally idempotent: configure can be re-run without changing the
# DuckDB submodule after the first successful patch. On non-MSVC compilers the
# guarded branch was already inactive, so selecting the fallback is equivalent.

set(_parser_tools_fmt_header
    "${CMAKE_CURRENT_LIST_DIR}/../duckdb/third_party/fmt/include/fmt/format.h")

if(NOT EXISTS "${_parser_tools_fmt_header}")
  message(FATAL_ERROR "Bundled fmt header not found: ${_parser_tools_fmt_header}")
endif()

file(READ "${_parser_tools_fmt_header}" _parser_tools_fmt_contents)
string(FIND "${_parser_tools_fmt_contents}" "#ifdef _SECURE_SCL" _parser_tools_guard)

if(_parser_tools_guard GREATER -1)
  string(REPLACE
    "#ifdef _SECURE_SCL"
    "#if 0 // stdext::checked_array_iterator was removed from newer MSVC"
    _parser_tools_fmt_contents
    "${_parser_tools_fmt_contents}")
  file(WRITE "${_parser_tools_fmt_header}" "${_parser_tools_fmt_contents}")
  message(STATUS "Patched bundled fmt to avoid stdext::checked_array_iterator")
else()
  message(STATUS "Bundled fmt already uses the portable iterator implementation")
endif()
