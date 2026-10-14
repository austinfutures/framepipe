// args.hpp — minimal "--key value" / "--key=value" / "--flag" parser.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fp {

class ArgParser {
 public:
  ArgParser(int argc, char** argv, std::set<std::string> known, std::set<std::string> flags)
      : known_(std::move(known)), flags_(std::move(flags)) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "-h" || a == "--help") {
        help_ = true;
        continue;
      }
      if (a.rfind("--", 0) != 0) die("unexpected argument: " + a);
      std::string k = a.substr(2), v;
      auto eq = k.find('=');
      if (eq != std::string::npos) {
        v = k.substr(eq + 1);
        k = k.substr(0, eq);
      } else if (flags_.count(k)) {
        v = "1";
      } else if (i + 1 < argc) {
        v = argv[++i];
      } else {
        die("missing value for --" + k);
      }
      if (!known_.count(k) && !flags_.count(k)) die("unknown option --" + k);
      kv_[k] = v;
    }
  }

  bool help() const { return help_; }
  bool flag(const std::string& k) const {
    auto it = kv_.find(k);
    return it != kv_.end() && it->second != "0";
  }
  std::string str(const std::string& k, const std::string& def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  long num(const std::string& k, long def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    if (!end || *end) die("--" + k + " expects an integer, got " + it->second);
    return v;
  }
  double real(const std::string& k, double def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    if (!end || *end) die("--" + k + " expects a number, got " + it->second);
    return v;
  }
  // "2,3" -> {2,3}; "" -> {}
  std::vector<int> int_list(const std::string& k) const {
    std::vector<int> out;
    std::stringstream ss(str(k, ""));
    std::string tok;
    while (std::getline(ss, tok, ','))
      if (!tok.empty()) out.push_back(std::atoi(tok.c_str()));
    return out;
  }

  [[noreturn]] static void die(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n", msg.c_str());
    std::exit(2);
  }

 private:
  std::set<std::string> known_, flags_;
  std::map<std::string, std::string> kv_;
  bool help_ = false;
};

}  // namespace fp
