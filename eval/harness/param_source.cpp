// SCAFFOLDING
#include "eval/harness/param_source.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>

#ifndef ACCEL_SRC_DIR
#define ACCEL_SRC_DIR ""
#endif

namespace accelerator_codegen::aux_state::harness {
namespace {

// The generator's output is JSON of objects, arrays, strings and numbers.
struct Json {
  std::map<std::string, Json> object;
  std::vector<Json> array;
  std::string text; // a string's value, a number's text

  const Json &At(const std::string &key) const {
    const auto it = object.find(key);
    if (it == object.end()) {
      throw std::runtime_error("parameter JSON has no \"" + key + "\"");
    }
    return it->second;
  }
};

class JsonReader {
public:
  explicit JsonReader(const std::string &input) : in(input) {}

  Json Read() {
    Json value = Value();
    SkipSpace();
    if (at != in.size()) {
      Fail("trailing text");
    }
    return value;
  }

private:
  [[noreturn]] void Fail(const std::string &what) const {
    throw std::runtime_error("parameter JSON: " + what + " at offset " + std::to_string(at));
  }
  void SkipSpace() {
    while (at < in.size() && std::isspace(static_cast<unsigned char>(in[at]))) {
      ++at;
    }
  }
  void Expect(char c) {
    SkipSpace();
    if (at >= in.size() || in[at] != c) {
      Fail(std::string("expected '") + c + "'");
    }
    ++at;
  }
  bool Take(char c) {
    SkipSpace();
    if (at < in.size() && in[at] == c) {
      ++at;
      return true;
    }
    return false;
  }

  std::string String() {
    Expect('"');
    std::string out;
    while (at < in.size() && in[at] != '"') {
      char c = in[at++];
      if (c == '\\') {
        if (at >= in.size()) {
          Fail("unterminated escape");
        }
        c = in[at++];
        switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'u': { // the generator escapes only non-ASCII; keep the code point as UTF-8
          if (at + 4 > in.size()) {
            Fail("short \\u escape");
          }
          const unsigned cp = static_cast<unsigned>(std::stoul(in.substr(at, 4), nullptr, 16));
          at += 4;
          if (cp < 0x80) {
            out += static_cast<char>(cp);
          } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          }
          continue;
        }
        default: break; // \" \\ \/
        }
      }
      out += c;
    }
    Expect('"');
    return out;
  }

  Json Value() {
    SkipSpace();
    Json value;
    if (at >= in.size()) {
      Fail("unexpected end");
    }
    if (in[at] == '{') {
      ++at;
      if (!Take('}')) {
        do {
          const auto key = String();
          Expect(':');
          value.object[key] = Value();
        } while (Take(','));
        Expect('}');
      }
    } else if (in[at] == '[') {
      ++at;
      if (!Take(']')) {
        do {
          value.array.push_back(Value());
        } while (Take(','));
        Expect(']');
      }
    } else if (in[at] == '"') {
      value.text = String();
    } else {
      while (at < in.size() && (std::isalnum(static_cast<unsigned char>(in[at])) || in[at] == '-' || in[at] == '+' ||
                                in[at] == '.')) {
        value.text += in[at++];
      }
      if (value.text.empty()) {
        Fail("unexpected character");
      }
    }
    return value;
  }

  const std::string &in;
  std::size_t at = 0;
};

std::string EnvOr(const char *name, const std::string &fallback) {
  const char *value = std::getenv(name);
  return value && *value ? value : fallback;
}

std::string Slurp(std::FILE *file) {
  std::string out;
  char buffer[4096];
  while (const auto n = std::fread(buffer, 1, sizeof buffer, file)) {
    out.append(buffer, n);
  }
  return out;
}

std::string ParamJson(const std::string &workload) {
  if (const char *path = std::getenv("ACCEL_PARAM_FILE"); path && *path) {
    std::ifstream in(path);
    if (!in.good()) {
      throw std::runtime_error(std::string("cannot read ACCEL_PARAM_FILE ") + path);
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  }
  const auto src = EnvOr("ACCEL_SRC_DIR", ACCEL_SRC_DIR);
  if (src.empty()) {
    throw std::runtime_error("no parameter source: set ACCEL_PARAM_FILE or ACCEL_SRC_DIR");
  }
  const auto sets = EnvOr("ACCEL_PARAM_SETS", "8");
  const auto seed = EnvOr("ACCEL_PARAM_SEED", std::to_string(std::random_device{}() % 1000000000u));
  const auto command = "python3 '" + src + "/eval/bench/" + workload + "/gen_params.py' " +
                       std::to_string(std::stoi(sets)) + " " + std::to_string(std::stoll(seed));
  std::unique_ptr<std::FILE, int (*)(std::FILE *)> pipe(popen(command.c_str(), "r"), pclose);
  if (!pipe) {
    throw std::runtime_error("cannot run " + command);
  }
  auto text = Slurp(pipe.get());
  if (pclose(pipe.release()) != 0 || text.empty()) {
    throw std::runtime_error("parameter generator failed: " + command);
  }
  return text;
}

} // namespace

const std::vector<std::string> &DrawArgumentLists(const std::string &workload, int query_id) {
  static std::map<std::pair<std::string, int>, std::vector<std::string>> drawn;
  const auto key = std::make_pair(workload, query_id);
  auto it = drawn.find(key);
  if (it != drawn.end()) {
    return it->second;
  }
  const auto json_text = ParamJson(workload);
  const auto root = JsonReader(json_text).Read();
  const auto query = std::to_string(query_id);
  const auto &queries = root.At("queries").object;
  if (!queries.count(query)) {
    throw std::runtime_error(workload + " q" + query + ": the parameter source draws nothing for it");
  }
  std::vector<std::string> lists;
  for (const auto &set : queries.at(query).At("sets").array) {
    lists.push_back(set.At("cpp").text);
  }
  std::cerr << "[params] " << workload << " q" << query << " seed=" << root.At("seed").text << " sets=" << lists.size()
            << "\n";
  return drawn.emplace(key, std::move(lists)).first->second;
}

} // namespace accelerator_codegen::aux_state::harness
