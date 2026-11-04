#pragma once
// Structured messages and a small C++20 Expected<T>. Errors are values: no
// exception ever crosses the engine API.
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace pixel3d {

enum class Severity { Info, Warning, Error };

struct Message {
  std::string code;         // stable machine-readable id, e.g. "io.not_found"
  std::string text;         // human readable
  Severity severity = Severity::Error;
  nlohmann::json where;     // optional location payload (view, pixels, part, ...)
};

std::string_view to_string(Severity);

struct Unexpected {
  Message error;
};

Unexpected make_error(std::string code, std::string text, nlohmann::json where = {});

template <class T>
class Expected {
 public:
  Expected(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT: implicit by design
  Expected(Unexpected e) : v_(std::in_place_index<1>, std::move(e.error)) {}  // NOLINT

  bool has_value() const { return v_.index() == 0; }
  explicit operator bool() const { return has_value(); }

  T& value() & { return std::get<0>(v_); }
  const T& value() const& { return std::get<0>(v_); }
  T&& value() && { return std::get<0>(std::move(v_)); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Message& error() const { return std::get<1>(v_); }

 private:
  std::variant<T, Message> v_;
};

void to_json(nlohmann::json& j, const Message& m);

}  // namespace pixel3d
