// MIT License © 2025 Binary Dice Games
/**
 * @file error.hpp
 * @brief Exception type raised for non-`_OK` `wish_*`/`rmi_*`/`bison_*`
 *        return codes.
 */
#pragma once

#include <wish_client_c.h>

#include <stdexcept>
#include <string>

namespace bdg::wish::binding {

/** @brief Raised when a `wish_*`/`rmi_*`/`bison_*` C ABI call returns a non-zero error code. */
class error : public std::runtime_error {
 public:
  error(int code, const std::string& context)
      : std::runtime_error(context), code_(code) {}

  /** @brief The raw `wish_error`/`rmi_error`/`bison_error` code. */
  int code() const noexcept { return code_; }

 private:
  int code_;
};

namespace detail {

inline const char* wish_error_message(wish_error rc) {
  switch (rc) {
    case WISH_ERR_NULL:
      return "null handle or pointer";
    case WISH_ERR_NOT_FOUND:
      return "named proxy or resource not found";
    case WISH_ERR_TRANSPORT:
      return "transport connection failed";
    case WISH_ERR_EXCEPTION:
      return "internal C++ exception";
    case WISH_ERR_AMBIGUOUS:
      return "app name matches more than one registered app; use the fully-qualified name";
    default:
      return "unknown error";
  }
}

inline const char* rmi_error_message(rmi_error rc) {
  switch (rc) {
    case RMI_ERR_NULL:
      return "null handle or pointer";
    case RMI_ERR_INVALID_STATE:
      return "operation invalid for current state (e.g. not connected)";
    case RMI_ERR_TIMEOUT:
      return "request timed out";
    case RMI_ERR_REMOTE_EXCEPTION:
      return "server raised an exception";
    case RMI_ERR_TRANSPORT:
      return "transport error";
    case RMI_ERR_EXCEPTION:
      return "internal C++ exception";
    default:
      return "unknown error";
  }
}

inline const char* bison_error_message(bison_error rc) {
  switch (rc) {
    case BISON_ERR_NULL:
      return "null handle or pointer";
    case BISON_ERR_TYPE:
      return "field holds a different type than requested";
    case BISON_ERR_NOT_FOUND:
      return "method or field not found";
    case BISON_ERR_DUPLICATE:
      return "duplicate class or method";
    case BISON_ERR_EXCEPTION:
      return "internal C++ exception";
    case BISON_ERR_PARSE:
      return "input string failed to parse (JSON / YAML)";
    default:
      return "unknown error";
  }
}

// Appends client-specific detail (wish_last_error) when a client handle is
// available, since it is more precise than the generic per-code message.
inline void throw_if_wish_error(wish_error rc, const std::string& context, wish_client_handle client = nullptr) {
  if (rc == WISH_OK) return;
  std::string msg = context + ": " + wish_error_message(rc);
  if (client) {
    const char* detail = wish_last_error(client);
    if (detail && *detail) msg += " (" + std::string(detail) + ")";
  }
  throw error(rc, msg);
}

inline void throw_if_rmi_error(rmi_error rc, const std::string& context) {
  if (rc == RMI_OK) return;
  throw error(rc, context + ": " + rmi_error_message(rc));
}

inline void throw_if_bison_error(bison_error rc, const std::string& context) {
  if (rc == BISON_OK) return;
  throw error(rc, context + ": " + bison_error_message(rc));
}

}  // namespace detail
}  // namespace bdg::wish::binding
