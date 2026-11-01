# Thin wrapper so the common commands are short. The real build is CMake.
.PHONY: all debug release tsan test check format clean

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

clean:
	rm -rf build
