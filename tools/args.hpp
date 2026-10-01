#pragma once
// Tiny --key value / --flag argument parser shared by the tools.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace obl::tools {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) == 0) {
        const auto eq = a.find('=');
        if (eq != std::string::npos) kv_[a.substr(2, eq - 2)] = a.substr(eq + 1);
        else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) kv_[a.substr(2)] = argv[++i];
        else kv_[a.substr(2)] = "1";
      } else {
        positional_.push_back(a);
      }
    }
  }

  bool has(const std::string& k) const { return kv_.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  double num(const std::string& k, double def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::strtod(it->second.c_str(), nullptr);
  }
  std::uint64_t u64(const std::string& k, std::uint64_t def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : static_cast<std::uint64_t>(std::strtod(it->second.c_str(), nullptr));
  }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::string> kv_;
  std::vector<std::string> positional_;
};

}  // namespace obl::tools
