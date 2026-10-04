PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=parser_tools
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Workaround for a quoting bug in extension-ci-tools <= v1.5.x: TEST_PATH is
# defined with baked-in quotes (TEST_PATH="/test/unittest"), so the test
# invocation becomes ./build/release/"/test/unittest", which fails with
# Error 127 on Windows (Git Bash) — the test binary never runs. Fixed
# upstream in v2.0-cyanoptera via the RUN_TEST macro; override here until
# ci_tools can move past v1.5.x. (On POSIX this is identical to the stock
# recipe: ./build/release//test/unittest.)
test_release_internal:
	./build/release/test/unittest "$(TESTS_BASE_DIRECTORY)*"
