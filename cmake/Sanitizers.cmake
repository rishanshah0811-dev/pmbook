# PM_SANITIZE is a comma list passed to -fsanitize=, e.g. "address,undefined"
# or "thread". Applied to everything (deps too), because sanitizers need the
# whole program instrumented to avoid false positives.
set(PM_SANITIZE "" CACHE STRING "Sanitizers to enable (-fsanitize=...)")
if(PM_SANITIZE)
  message(STATUS "Sanitizers: ${PM_SANITIZE}")
  add_compile_options(-fsanitize=${PM_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${PM_SANITIZE})
  if(PM_SANITIZE MATCHES "undefined")
    # Make UB a hard failure so tests can't pass while UB happens.
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()
