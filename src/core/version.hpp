#pragma once

#include <string_view>

namespace pm {

// Returns the engine version, e.g. "0.1.0".
std::string_view version() noexcept;

}  // namespace pm
