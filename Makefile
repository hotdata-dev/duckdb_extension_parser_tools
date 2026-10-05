PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=parser_tools
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Windows vs2019 static leg: the extension-ci-tools workflow's vcvars64.bat
# path (VS2022 Enterprise) no longer exists on current windows-latest
# runners, so CMake silently falls back to GCC and the built unittest binary
# cannot be launched from Git Bash (Error 127, deterministic across runs).
# Skip test execution on windows_amd64 only; Windows coverage is retained via
# windows_amd64_mingw, where the same suite passes. Re-enable once upstream
# fixes the VS path. (Must precede the ci-tools include: SKIP_TESTS is read
# at include time.)
ifeq ($(DUCKDB_PLATFORM),windows_amd64)
SKIP_TESTS=1
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Override the test invocation: the stock v1.4.4/v1.5.x recipe bakes quotes
# into TEST_PATH ("./build/release/\"/test/unittest\""), which is fragile
# under Windows (Git Bash). This form is identical on POSIX and verified
# working on the mingw Windows job.
test_release_internal:
	./build/release/test/unittest "$(TESTS_BASE_DIRECTORY)*"
