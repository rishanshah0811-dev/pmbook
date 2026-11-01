# Thin wrapper so the common commands are short. The real build is CMake.
.PHONY: all debug release tsan test check format format-check clean replay-sample

all: release

debug:
	cmake --preset debug && cmake --build --preset debug

release:
	cmake --preset release && cmake --build --preset release

tsan:
	cmake --preset tsan && cmake --build --preset tsan

test: debug
	ctest --preset debug

# Everything: sanitizer builds + release, all tests, formatting check.
check:
	cmake --workflow --preset check
	cmake --workflow --preset check-tsan
	cmake --workflow --preset check-release
	@$(MAKE) --no-print-directory format-check

format:
	clang-format -i $$(git ls-files '*.cpp' '*.hpp')

format-check:
	clang-format --dry-run --Werror $$(git ls-files '*.cpp' '*.hpp')

# Replay the shipped 12-minute recording and check it reproduces the live run.
replay-sample: release
	./build/release/pmbook replay --expect data/sample/live_summary.json data/sample/pm_*.jsonl.gz

clean:
	rm -rf build
