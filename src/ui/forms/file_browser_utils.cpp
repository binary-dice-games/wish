// MIT License © 2025 Binary Dice Games
/// @file file_browser_utils.cpp
/// @brief Implementation of shared file-listing helpers -- see the header
/// for the FileDialog/Mc/PixViewer duplication this factors out.
#include "file_browser_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bdg::wish {

using namespace bison;

std::string icon_for_entry(const std::string& name, const std::string& type) {
  if (type == "dir")
    return "folder";

  auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 == name.size())
    return "file";
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  static const std::vector<std::string> kImageExts{"png", "jpg", "jpeg", "gif", "bmp", "webp", "tga", "svg"};
  static const std::vector<std::string> kAudioExts{"mp3", "wav", "ogg", "flac", "m4a", "aac"};
  static const std::vector<std::string> kCodeExts{
      "cpp", "hpp", "c", "h", "cc", "cs", "py", "js", "ts", "java", "go", "rs",
      "sh", "json", "yaml", "yml", "xml", "html", "css"};
  static const std::vector<std::string> kDocumentExts{"txt", "md", "pdf", "doc", "docx", "rtf", "log"};

  auto contains = [&](const std::vector<std::string>& exts) {
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
  };

  if (contains(kImageExts))
    return "image";
  if (contains(kAudioExts))
    return "audio";
  if (contains(kCodeExts))
    return "code";
  if (contains(kDocumentExts))
    return "document";
  return "file";
}

ui_element_ptr make_name_cell(const std::string& name, const std::string& type, const std::string& display_name) {
  ui_element_ptr icon_row = ui_element_ptr::create("wish"_key, "HorizontalLayout"_key);
  icon_row["spacing"_key] = 6.0f;
  icon_row["order"_key] = int32_t{0};

  ui_element_ptr icon_img = ui_element_ptr::create("wish"_key, "Image"_key);
  icon_img["src"_key] = "res/icons/" + icon_for_entry(name, type) + ".png";
  icon_img["__auto_size_to_font__"_key] = true;
  // The icon PNGs are white/monochrome so they can be tinted; without this
  // they're invisible against the light theme's white background (see
  // render_image()'s "__tint_to_text_color__" handling).
  icon_img["__tint_to_text_color__"_key] = true;
  icon_img["order"_key] = int32_t{0};

  ui_element_ptr name_lbl = ui_element_ptr::create("wish"_key, "Label"_key);
  name_lbl["text"_key] = display_name;
  name_lbl["order"_key] = int32_t{1};

  auto icon_row_children = dynamic_ptr{key_t{0U}, {}};
  (*icon_row_children)[size_t{0}] = dynamic_ptr{icon_img};
  (*icon_row_children)[size_t{1}] = dynamic_ptr{name_lbl};
  icon_row["children"_key] = icon_row_children;

  return icon_row;
}

bool ascii_ci_less(const std::string& a, const std::string& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
    unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb)
      return ca < cb;
  }
  return a.size() < b.size();
}

double parse_display_size(const std::string& text) {
  if (text.empty())
    return -1.0;

  char* end = nullptr;
  double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str())
    return -1.0;

  // Skip the space between the number and unit (format_bytes() always
  // separates them, e.g. "12.3 KB").
  while (*end == ' ')
    ++end;

  std::string unit = end;
  for (auto& c : unit)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  for (size_t i = 0; i < std::size(kUnits); ++i) {
    if (unit == kUnits[i])
      return value * std::pow(1024.0, static_cast<double>(i));
  }
  return value;
}

namespace {

#if !defined(_WIN32)
// Forks/execs `argv` (nullptr-terminated) and waits for it, returning true
// on a clean exit(0). `argv[0]` is resolved via PATH (execvp).
bool run_and_wait(char* const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// True under WSL (Windows Subsystem for Linux): still plain Linux
// (_WIN32 is not defined), but `xdg-open` is typically absent since there's
// no Linux desktop session -- the host file manager is reached via
// `explorer.exe` instead. Detected the same way `wslpath`/util-linux
// tooling does: the kernel release string identifies itself.
bool running_under_wsl() {
  std::ifstream in("/proc/version");
  std::string line;
  std::getline(in, line);
  auto lower = line;
  for (auto& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower.find("microsoft") != std::string::npos;
}

// WSL fallback: convert `path` to its Windows form via `wslpath -w`, then
// hand it to the host's `explorer.exe` (present on PATH in every WSL
// distro). Returns "" if the conversion failed.
std::string wsl_windows_path(const std::filesystem::path& path) {
  std::string cmd = "wslpath -w " + path.string();
  // path is a resolved sandbox-relative filesystem path, never raw client
  // input, so shelling out via popen() here does not admit injection.
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return {};
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), pipe))
    out += buf;
  pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
}
#endif

} // namespace

bool open_in_host_explorer(const std::filesystem::path& path) {
#if defined(_WIN32)
  auto wide = path.wstring();
  auto result =
      reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"explore", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
#else
  std::string path_str = path.string();
  char* xdg_argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(path_str.c_str()), nullptr};
  if (run_and_wait(xdg_argv))
    return true;

  // xdg-open is commonly missing on WSL (no Linux desktop session) --
  // fall back to the Windows host's own Explorer via explorer.exe.
  if (running_under_wsl()) {
    std::string win_path = wsl_windows_path(path);
    if (!win_path.empty()) {
      char* explorer_argv[] = {const_cast<char*>("explorer.exe"), const_cast<char*>(win_path.c_str()), nullptr};
      // explorer.exe returns a non-zero exit status even on a successful
      // open (a long-standing Windows quirk) -- treat "we could launch it
      // at all" as success rather than trusting its exit code.
      pid_t pid = fork();
      if (pid == 0) {
        execvp(explorer_argv[0], explorer_argv);
        _exit(127);
      }
      if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) && WEXITSTATUS(status) != 127;
      }
    }
  }
  return false;
#endif
}

} // namespace bdg::wish
