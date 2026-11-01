#pragma once
// Compares two run summaries (see summary.hpp) asset by asset.

#include <string>
#include <string_view>
#include <vector>

namespace pm {

// Returns a list of human-readable differences in per-asset state and
// state_hash between two summary JSON documents. Empty list = identical books.
std::vector<std::string> compare_summaries(std::string_view expected_json,
                                           std::string_view actual_json);

}  // namespace pm
