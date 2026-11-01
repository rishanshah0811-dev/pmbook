#include "app/summary_compare.hpp"

#include <simdjson.h>

#include <map>
#include <utility>

namespace pm {

namespace {

using AssetMap = std::map<std::string, std::pair<std::string, std::string>>;  // id -> (state, hash)

bool load(std::string_view json, AssetMap& out, std::string& err) {
  simdjson::dom::parser parser;
  simdjson::dom::element doc;
  if (parser.parse(json.data(), json.size()).get(doc)) {
    err = "not valid JSON";
    return false;
  }
  simdjson::dom::object assets;
  if (doc["assets"].get(assets)) {
    err = "no \"assets\" object";
    return false;
  }
  for (auto [key, val] : assets) {
    std::string_view state, hash;
    if (val["state"].get(state) || val["state_hash"].get(hash)) {
      err = "asset entry missing state/state_hash";
      return false;
    }
    out[std::string(key)] = {std::string(state), std::string(hash)};
  }
  return true;
}

}  // namespace

std::vector<std::string> compare_summaries(std::string_view expected_json,
                                           std::string_view actual_json) {
  std::vector<std::string> diffs;
  AssetMap exp, act;
  std::string err;
  if (!load(expected_json, exp, err)) return {"expected: " + err};
  if (!load(actual_json, act, err)) return {"actual: " + err};
  for (const auto& [id, e] : exp) {
    auto it = act.find(id);
    if (it == act.end()) {
      diffs.push_back(id.substr(0, 12) + "...: missing in actual");
    } else if (it->second != e) {
      diffs.push_back(id.substr(0, 12) + "...: expected " + e.first + "/" + e.second + ", got " +
                      it->second.first + "/" + it->second.second);
    }
  }
  for (const auto& [id, a] : act) {
    if (!exp.count(id)) diffs.push_back(id.substr(0, 12) + "...: unexpected asset");
  }
  return diffs;
}

}  // namespace pm
