#pragma once

#include <string>
#include <vector>

namespace util {

std::string base64_encode(const std::string &in);
std::string base64_decode(const std::string &in);
std::vector<std::string> split_args(const std::string &cmdline);  // splits while honoring double quotes
bool has_flag(const std::vector<std::string> &args, const std::string &flag);
std::string flag_value(const std::vector<std::string> &args, const std::string &flag, const std::string &def = "");
bool constant_time_eq(const std::string &a, const std::string &b);
std::string to_lower(std::string s);
std::string hex(const unsigned char *p, size_t n);

}  // namespace util
