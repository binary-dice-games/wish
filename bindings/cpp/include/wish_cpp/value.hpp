// MIT License © 2025 Binary Dice Games
/**
 * @file value.hpp
 * @brief Header-only RAII wrapper around `bison_handle`.
 *
 * `bdg::bison::dynamic` (extern/bison/src/bison/bison_object.hpp) is the
 * ergonomic C++ value type used by the native, linked wish client -- but it
 * is compiled/linked library code, not header-only. This binding only
 * depends on `bison_c.h`/`rmi_c.h`/`wish_client_c.h` (declarations) plus the
 * prebuilt `wish_client_dll` shared library, so it needs its own thin,
 * header-only stand-in built directly on the `bison_*` C ABI functions.
 */
#pragma once

#include "error.hpp"
#include "key.hpp"

#include <bison_c.h>

#include <optional>
#include <string>
#include <utility>

namespace bdg::wish::binding {

/**
 * @brief RAII wrapper around a `bison_handle` -- a reference-counted map/array
 *        of typed fields, used for proxy `set()`/`get()`/`call()` payloads,
 *        event parameters, and connect params.
 */
class value {
 public:
  /** @brief A new, empty object (mirrors `bison::dynamic{}`). */
  value() : h_(bison_create(0)) {}

  /** @brief Adopts ownership of an existing handle (may be `nullptr`). */
  static value adopt(bison_handle h) { return value(h); }

  /** @brief `nullptr`-handle value -- distinct from an empty object; used where the C ABI treats `NULL` specially (e.g. "no projection"). */
  static value null() { return value(nullptr); }

  value(const value& other) : h_(other.h_ ? bison_add_ref(other.h_) : nullptr) {}
  value(value&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }

  value& operator=(const value& other) {
    if (this != &other) {
      bison_handle new_h = other.h_ ? bison_add_ref(other.h_) : nullptr;
      bison_release(h_);
      h_ = new_h;
    }
    return *this;
  }

  value& operator=(value&& other) noexcept {
    if (this != &other) {
      bison_release(h_);
      h_ = other.h_;
      other.h_ = nullptr;
    }
    return *this;
  }

  ~value() { bison_release(h_); }

  bool valid() const noexcept { return h_ != nullptr; }
  bison_handle handle() const noexcept { return h_; }

  /** @brief Releases ownership to the caller (e.g. to hand off to a C ABI call that takes ownership); this value becomes null. */
  bison_handle release() noexcept {
    bison_handle h = h_;
    h_ = nullptr;
    return h;
  }

  value clone() const { return value(h_ ? bison_clone(h_) : nullptr); }

  // ── Import / export ───────────────────────────────────────────────────

  static value parse_json(const std::string& json) {
    bison_handle h = bison_from_json(json.c_str());
    if (!h) throw error(BISON_ERR_PARSE, "value::parse_json: invalid JSON");
    return value(h);
  }

  static value parse_yaml(const std::string& yaml) {
    bison_handle h = bison_from_yaml(yaml.c_str());
    if (!h) throw error(BISON_ERR_PARSE, "value::parse_yaml: invalid YAML");
    return value(h);
  }

  std::string to_json(int indent = -1) const {
    char* out = nullptr;
    detail::throw_if_bison_error(bison_to_json(h_, indent, &out), "value::to_json");
    std::string result = out ? out : "";
    bison_free_string(out);
    return result;
  }

  std::string to_yaml() const {
    char* out = nullptr;
    detail::throw_if_bison_error(bison_to_yaml(h_, &out), "value::to_yaml");
    std::string result = out ? out : "";
    bison_free_string(out);
    return result;
  }

  // ── Scalar field setters ──────────────────────────────────────────────

  value& set_int(key_t name, int32_t v) {
    detail::throw_if_bison_error(bison_set_int(h_, name, v), "value::set_int");
    return *this;
  }
  value& set_float(key_t name, float v) {
    detail::throw_if_bison_error(bison_set_float(h_, name, v), "value::set_float");
    return *this;
  }
  value& set_bool(key_t name, bool v) {
    detail::throw_if_bison_error(bison_set_bool(h_, name, v ? 1 : 0), "value::set_bool");
    return *this;
  }
  value& set_string(key_t name, const std::string& v) {
    detail::throw_if_bison_error(bison_set_string(h_, name, v.c_str()), "value::set_string");
    return *this;
  }
  value& set_object(key_t name, const value& v) {
    detail::throw_if_bison_error(bison_set_object(h_, name, v.h_), "value::set_object");
    return *this;
  }

  // ── Scalar field getters ──────────────────────────────────────────────

  std::optional<int32_t> get_int(key_t name) const {
    int32_t out = 0;
    if (bison_get_int(h_, name, &out) != BISON_OK) return std::nullopt;
    return out;
  }
  std::optional<float> get_float(key_t name) const {
    float out = 0.0f;
    if (bison_get_float(h_, name, &out) != BISON_OK) return std::nullopt;
    return out;
  }
  std::optional<bool> get_bool(key_t name) const {
    int out = 0;
    if (bison_get_bool(h_, name, &out) != BISON_OK) return std::nullopt;
    return out != 0;
  }
  std::optional<std::string> get_string(key_t name) const {
    size_t len = 0;
    if (bison_get_string(h_, name, nullptr, 0, &len) != BISON_OK) return std::nullopt;
    std::string out(len, '\0');
    if (len > 0 && bison_get_string(h_, name, out.data(), len + 1, nullptr) != BISON_OK) return std::nullopt;
    return out;
  }
  std::optional<value> get_object(key_t name) const {
    bison_handle out = nullptr;
    if (bison_get_object(h_, name, &out) != BISON_OK) return std::nullopt;
    return value(out);
  }

  // ── Array-index field access ──────────────────────────────────────────

  value& set_int_at(size_t index, int32_t v) {
    detail::throw_if_bison_error(bison_set_int_at(h_, index, v), "value::set_int_at");
    return *this;
  }
  value& set_float_at(size_t index, float v) {
    detail::throw_if_bison_error(bison_set_float_at(h_, index, v), "value::set_float_at");
    return *this;
  }
  value& set_string_at(size_t index, const std::string& v) {
    detail::throw_if_bison_error(bison_set_string_at(h_, index, v.c_str()), "value::set_string_at");
    return *this;
  }

  std::optional<int32_t> get_int_at(size_t index) const {
    int32_t out = 0;
    if (bison_get_int_at(h_, index, &out) != BISON_OK) return std::nullopt;
    return out;
  }
  std::optional<float> get_float_at(size_t index) const {
    float out = 0.0f;
    if (bison_get_float_at(h_, index, &out) != BISON_OK) return std::nullopt;
    return out;
  }
  std::optional<std::string> get_string_at(size_t index) const {
    size_t len = 0;
    if (bison_get_string_at(h_, index, nullptr, 0, &len) != BISON_OK) return std::nullopt;
    std::string out(len, '\0');
    if (len > 0 && bison_get_string_at(h_, index, out.data(), len + 1, nullptr) != BISON_OK) return std::nullopt;
    return out;
  }

  /** @brief Number of array-like (numeric-key) elements. */
  size_t size() const { return bison_size(h_); }

  // ── Field-assignment sugar: `v["name"_key] = 1.0f;` ───────────────────

  class field_ref {
   public:
    field_ref(value& owner, key_t name) : owner_(owner), name_(name) {}

    field_ref& operator=(int32_t v) { owner_.set_int(name_, v); return *this; }
    field_ref& operator=(float v) { owner_.set_float(name_, v); return *this; }
    field_ref& operator=(double v) { owner_.set_float(name_, static_cast<float>(v)); return *this; }
    field_ref& operator=(bool v) { owner_.set_bool(name_, v); return *this; }
    field_ref& operator=(const std::string& v) { owner_.set_string(name_, v); return *this; }
    field_ref& operator=(const char* v) { owner_.set_string(name_, v); return *this; }
    field_ref& operator=(const value& v) { owner_.set_object(name_, v); return *this; }

   private:
    value& owner_;
    key_t name_;
  };

  field_ref operator[](key_t name) { return field_ref(*this, name); }

 private:
  explicit value(bison_handle h) : h_(h) {}

  bison_handle h_;
};

}  // namespace bdg::wish::binding
