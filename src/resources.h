#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace resources {

nlohmann::json list();
nlohmann::json templates();
// null if unknown; {"__error": {code, message}} if the resource exists but could not be produced.
nlohmann::json read(const std::string &uri);
// subscriptions/listen support: resources whose updates are notified, and their current version stamp.
bool subscribable(const std::string &uri);
uint64_t version(const std::string &uri);
// completion/complete for a resource template argument.
std::vector<std::string> complete(const std::string &uri_template, const std::string &arg, const std::string &value);
std::string instructions();                    // `instructions` field of initialize

}  // namespace resources
