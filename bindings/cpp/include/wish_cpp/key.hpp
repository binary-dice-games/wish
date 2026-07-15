// MIT License © 2025 Binary Dice Games
/**
 * @file key.hpp
 * @brief Compile-time field/method/class name hashing for the header-only
 *        C++ binding.
 *
 * `wish_key()` / `bison_key()` (the C ABI) compute the FNV-1a hash of a name
 * at runtime, once per call -- fine for Python/C#, which have no
 * compile-time evaluation. C++ does, so `key_t`'s `const char*` constructor
 * below reimplements the exact same algorithm as a `constexpr` function
 * (bit-identical to `bdg::bison::hash()` in extern/bison/src/bison/bison_common.hpp
 * and to `bison_key()`/`wish_key()`): a `"name"_key` literal is evaluated by
 * the compiler and costs nothing at runtime, with no shared-library call
 * needed at all. Only names that are truly known solely at runtime (e.g. a
 * dot-path string read from a config file) pay for hashing at runtime, via
 * the same constexpr function.
 */
#pragma once

#include <wish_client_c.h>

#include <cstddef>
#include <string>

namespace bdg::wish::binding {

/** @brief Pre-hashed name type; identical bit layout to `wish_hash`/`bison_hash`. */
using hash_t = wish_hash;

namespace detail {

// Bit-identical to bdg::bison::hash() (extern/bison/src/bison/bison_common.hpp):
// FNV-1a over the bytes of `input`, then the MSB is set so hashed names never
// collide with the small numeric field indices that share the same key space.
constexpr hash_t fnv1a(const char* input) noexcept {
  hash_t value = 0x811c9dc5u;
  const hash_t prime = 0x01000193u;
  while (*input) {
    value ^= static_cast<hash_t>(static_cast<unsigned char>(*input));
    value *= prime;
    ++input;
  }
  return value | 0x80000000u;
}

}  // namespace detail

/**
 * @brief Pre-hashed field/method/class name.
 *
 * Implicitly convertible to/from `hash_t` so it can be passed directly to
 * the `wish_*`/`bison_*`/`rmi_*` C ABI functions, which all take a plain
 * `wish_hash`/`bison_hash`.
 */
struct key_t {
  constexpr key_t() noexcept : id(0) {}
  constexpr key_t(hash_t v) noexcept : id(v) {}  // NOLINT(google-explicit-constructor)
  constexpr key_t(const char* name) noexcept : id(detail::fnv1a(name)) {}  // NOLINT(google-explicit-constructor)
  key_t(const std::string& name) noexcept : id(detail::fnv1a(name.c_str())) {}  // NOLINT(google-explicit-constructor)

  constexpr operator hash_t() const noexcept { return id; }  // NOLINT(google-explicit-constructor)

  hash_t id;
};

inline constexpr bool operator==(key_t lhs, key_t rhs) noexcept {
  return lhs.id == rhs.id;
}
inline constexpr bool operator!=(key_t lhs, key_t rhs) noexcept {
  return !(lhs == rhs);
}

/** @brief `"name"_key` literal -- evaluated at compile time, zero runtime cost. */
inline constexpr key_t operator""_key(const char* name, std::size_t) noexcept {
  return key_t{name};
}

}  // namespace bdg::wish::binding
