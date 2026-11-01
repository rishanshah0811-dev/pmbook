#pragma once
// One-line description of the machine and build, stamped into every results
// file so numbers are never quoted without saying where they came from.

#include <string>

namespace pm {

// e.g. "Apple M5 | 10 cores | 16 GB | Darwin 25.6.0 arm64"
std::string machine_description();
// e.g. "clang 23.1.4 | Release | LTO on"
std::string build_description();

}  // namespace pm
