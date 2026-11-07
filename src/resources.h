#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace resources {

nlohmann::json list();
nlohmann::json templates();
nlohmann::json read(const std::string &uri);  // null if unknown
std::string instructions();                    // `instructions` field of initialize

}  // namespace resources
