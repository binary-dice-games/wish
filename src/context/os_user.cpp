// MIT License © 2026 Binary Dice Games
/// @file os_user.cpp
/// @brief Implementation of `wish::os_user_name()`.
#include <context/os_user.hpp>

#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace bdg::wish {

namespace {

std::string strip_domain(std::string name) {
  if (auto pos = name.find_last_of('\\'); pos != std::string::npos)
    name.erase(0, pos + 1);
  if (auto pos = name.find('@'); pos != std::string::npos)
    name.erase(pos);
  return name;
}

std::string system_user_name() {
#if defined(_WIN32)
  wchar_t buf[257];
  DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
  if (!GetUserNameW(buf, &len) || len <= 1)
    return {};
  const int wlen = static_cast<int>(len - 1); // len counts the terminator
  const int n = WideCharToMultiByte(CP_UTF8, 0, buf, wlen, nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, wlen, out.data(), n, nullptr, nullptr);
  return out;
#else
  long size = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> buf(size > 0 ? static_cast<size_t>(size) : 16384);
  passwd pw{};
  passwd* result = nullptr;
  if (getpwuid_r(geteuid(), &pw, buf.data(), buf.size(), &result) == 0 && result && result->pw_name)
    return result->pw_name;
  return {};
#endif
}

} // namespace

std::string os_user_name() {
  std::string name = system_user_name();
  if (name.empty()) {
#if defined(_WIN32)
    const char* vars[] = {"USERNAME"};
#else
    const char* vars[] = {"USER", "LOGNAME"};
#endif
    for (const char* var : vars) {
      if (const char* v = std::getenv(var); v && *v) {
        name = v;
        break;
      }
    }
  }
  return strip_domain(std::move(name));
}

} // namespace bdg::wish
