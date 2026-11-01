# Strict warnings for OUR code only. Third-party code is not ours to fix,
# so these flags go on an interface target that only our targets link.
add_library(pm_warnings INTERFACE)
target_compile_options(pm_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
  $<$<BOOL:${PM_WERROR}>:-Werror>
)
