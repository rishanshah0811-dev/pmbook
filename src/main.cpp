#include <cstdio>

#include "core/version.hpp"

int main() {
  std::printf("pmbook %.*s\n", static_cast<int>(pm::version().size()), pm::version().data());
  return 0;
}
