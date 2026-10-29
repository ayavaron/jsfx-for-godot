#pragma once

#include <optional>
#include <string>
#include <utility>

namespace jsfx {

/// A failure with an optional 1-based source line in the original `.jsfx` file.
struct Error {
  std::string message;
  int line = 0;
};

/// Minimal expected-like result used across the main-thread API.
///
/// The core targets C++20, so `std::expected` (C++23) is not available yet.
template <typename T>
class Result {
 public:
  static Result success(T value) {
    Result result;
    result.value_.emplace(std::move(value));
    return result;
  }

  static Result failure(Error error) {
    Result result;
    result.error_ = std::move(error);
    return result;
  }

  static Result failure(std::string message, int line = 0) {
    return failure(Error{std::move(message), line});
  }

  explicit operator bool() const noexcept { return value_.has_value(); }
  bool ok() const noexcept { return value_.has_value(); }

  T& value() & { return *value_; }
  T&& value() && { return std::move(*value_); }
  const T& value() const& { return *value_; }

  const Error& error() const noexcept { return error_; }

 private:
  Result() = default;

  std::optional<T> value_;
  Error error_;
};

}  // namespace jsfx
